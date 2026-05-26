// include/oran/core/bounded_cache.hpp — generic LRU + TTL + byte-budget cache.
//
// `BoundedCache<Key, Value>` is the bounded-state primitive that spec 0011
// v1.1 (file-view system) and spec 0012 (tool scheduler) both reach for.
// Eviction order is documented in the type name, not hidden in a comment:
// LRU on access, TTL on age, byte-budget on payload.
//
// The cache is **single-threaded by contract**. Callers running on the
// agent strand (the dominant pattern in this codebase) need no extra
// synchronisation. Multi-threaded consumers wrap the cache in an explicit
// mutex; we did not add an internal lock because the first call sites all
// live on a single strand and an internal mutex hides cost from a caller
// that does not need it.
//
// Spec note: the spec sketch shows `get -> std::optional<Value>`. That
// shape is move-incompatible (`std::optional<unique_ptr<re2::RE2>>` is
// fine, but copying into the optional is required by `optional`'s ctor —
// and `unique_ptr` is non-copyable). We return `Value*` instead so a
// `BoundedCache<Pattern, unique_ptr<re2::RE2>>` works without forcing
// `shared_ptr`. The pointer is owned by the cache; treat it as invalidated
// after the next non-const operation.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include <oran/core/time.hpp>

namespace orangutan::core {

/// Default byte-size functor for `BoundedCache`. Returns 0 for every value,
/// effectively disabling the byte budget. Callers wanting byte-aware
/// eviction supply their own functor (e.g., a struct with
/// `std::size_t operator()(const std::string& s) const noexcept { return
/// s.size(); }`).
struct BoundedCacheNoByteBudget {
  template <class T>
  constexpr std::size_t operator()(const T&) const noexcept {
    return 0;
  }
};

template <class Key,
          class Value,
          class ByteSizeOf = BoundedCacheNoByteBudget,
          class Hash = std::hash<Key>,
          class KeyEqual = std::equal_to<Key>>
class BoundedCache {
public:
  struct Options {
    /// Hard cap on cache entry count. When `max_entries == 0` the entry
    /// cap is disabled; only TTL and byte-budget may evict.
    std::size_t max_entries{0};
    /// Hard cap on the *sum* of `ByteSizeOf{}(value)` across stored
    /// entries. `0` disables the byte budget.
    std::size_t max_bytes{0};
    /// Insert-based TTL. Entries are eligible for eviction after
    /// `now - insert_time > ttl`. `0` disables the TTL (entries live until
    /// LRU or byte-budget eviction).
    std::chrono::seconds ttl{0};
  };

  struct Stats {
    std::uint64_t hits{0};
    std::uint64_t misses{0};
    std::uint64_t evictions_lru{0};
    std::uint64_t evictions_ttl{0};
    std::uint64_t evictions_bytes{0};
    /// Items whose `ByteSizeOf{}(value)` exceeded `max_bytes` outright on
    /// `put`. The spec is explicit: the cache "refuses to cache items
    /// larger than its byte budget" rather than ejecting everything else
    /// to make room.
    std::uint64_t rejected_oversize{0};
    std::size_t current_entries{0};
    std::size_t current_bytes{0};
  };

  explicit BoundedCache(Options options,
                        ByteSizeOf size_of = ByteSizeOf{}) noexcept(std::is_nothrow_move_constructible_v<ByteSizeOf>)
      : options_{options}, size_of_(std::move(size_of)) {}

  BoundedCache(const BoundedCache&) = delete;
  BoundedCache& operator=(const BoundedCache&) = delete;
  BoundedCache(BoundedCache&&) noexcept = default;
  BoundedCache& operator=(BoundedCache&&) noexcept = default;

  /// Look up `key`. Returns a non-owning pointer to the stored value on
  /// hit (and refreshes the LRU position), or `nullptr` on miss / TTL
  /// expiry. The pointer is invalidated by the next call to `put`,
  /// `reap`, `erase_if`, or `clear`.
  [[nodiscard]] Value* get(const Key& key, Time now) {
    auto it = index_.find(key);
    if (it == index_.end()) {
      ++stats_.misses;
      return nullptr;
    }
    if (ttl_expired(*it->second, now)) {
      drop_locked(it, EvictionReason::ttl);
      ++stats_.misses;
      return nullptr;
    }
    entries_.splice(entries_.end(), entries_, it->second);
    ++stats_.hits;
    return &it->second->value;
  }

  /// Insert or refresh `key` -> `value`. When the new payload exceeds
  /// `max_bytes`, the entry is rejected (`rejected_oversize` increments)
  /// AND any prior entry under the same key is removed via the
  /// `EvictionReason::invalidated` path — a stale hit after a "refuses to
  /// cache" is worse than a clean miss, and the invalidation itself does
  /// not consume any `evictions_*` budget counter.
  void put(Key key, Value value, Time now) {
    const auto byte_size = static_cast<std::size_t>(size_of_(value));
    if (options_.max_bytes != 0 && byte_size > options_.max_bytes) {
      ++stats_.rejected_oversize;
      if (auto existing = index_.find(key); existing != index_.end()) {
        // Spec acceptance #7 (file-view cache safety) requires invalidation
        // on mutation; honour the same invariant for oversize rejection
        // without double-counting against the byte-pressure eviction tally.
        drop_locked(existing, EvictionReason::invalidated);
      }
      return;
    }
    if (auto existing = index_.find(key); existing != index_.end()) {
      drop_locked(existing, EvictionReason::invalidated);
    }
    entries_.push_back(Entry{.key = key, .value = std::move(value), .byte_size = byte_size, .insert_time = now});
    index_.emplace(std::move(key), std::prev(entries_.end()));
    stats_.current_entries = entries_.size();
    stats_.current_bytes += byte_size;
    enforce_bounds();
  }

  /// Sweep entries whose insert age exceeds `ttl`. Returns the number of
  /// entries evicted. No-op when `options_.ttl == 0s`.
  std::size_t reap(Time now) {
    if (options_.ttl == std::chrono::seconds{0}) {
      return 0;
    }
    std::size_t evicted = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (ttl_expired(*it, now)) {
        auto next = std::next(it);
        auto map_it = index_.find(it->key);
        if (map_it != index_.end()) {
          drop_locked(map_it, EvictionReason::ttl);
        }
        ++evicted;
        it = next;
      } else {
        ++it;
      }
    }
    return evicted;
  }

  /// Drop entries whose `(key, value)` pair matches `predicate`. Returns
  /// the number of entries removed. Policy eviction counters are preserved:
  /// this is explicit invalidation, not LRU / TTL / byte-budget eviction.
  template <class Predicate>
  std::size_t erase_if(Predicate&& predicate) {
    std::size_t erased = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (!std::invoke(predicate, std::as_const(it->key), std::as_const(it->value))) {
        ++it;
        continue;
      }
      const auto bytes = it->byte_size;
      index_.erase(it->key);
      it = entries_.erase(it);
      stats_.current_entries = entries_.size();
      stats_.current_bytes = (stats_.current_bytes >= bytes) ? stats_.current_bytes - bytes : 0;
      ++erased;
    }
    return erased;
  }

  /// Drop all entries; stats counters are preserved (they describe the
  /// cache's lifetime, not its current snapshot — see the spec).
  void clear() {
    entries_.clear();
    index_.clear();
    stats_.current_entries = 0;
    stats_.current_bytes = 0;
  }

  [[nodiscard]] const Stats& stats() const noexcept {
    return stats_;
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return entries_.size();
  }

  [[nodiscard]] std::size_t byte_size() const noexcept {
    return stats_.current_bytes;
  }

  [[nodiscard]] bool empty() const noexcept {
    return entries_.empty();
  }

  [[nodiscard]] const Options& options() const noexcept {
    return options_;
  }

private:
  struct Entry {
    Key key{};
    Value value{};
    std::size_t byte_size{0};
    Time insert_time{};
  };

  enum class EvictionReason {
    lru,
    ttl,
    bytes,
    invalidated
  };

  [[nodiscard]] bool ttl_expired(const Entry& entry, Time now) const noexcept {
    if (options_.ttl == std::chrono::seconds{0}) {
      return false;
    }
    const auto age = now.to_system_time_point() - entry.insert_time.to_system_time_point();
    return age > options_.ttl;
  }

  using ListIter = typename std::list<Entry>::iterator;

  void drop_locked(typename std::unordered_map<Key, ListIter, Hash, KeyEqual>::iterator map_it, EvictionReason reason) {
    const auto bytes = map_it->second->byte_size;
    entries_.erase(map_it->second);
    index_.erase(map_it);
    stats_.current_entries = entries_.size();
    stats_.current_bytes = (stats_.current_bytes >= bytes) ? stats_.current_bytes - bytes : 0;
    switch (reason) {
      case EvictionReason::lru:
        ++stats_.evictions_lru;
        break;
      case EvictionReason::ttl:
        ++stats_.evictions_ttl;
        break;
      case EvictionReason::bytes:
        ++stats_.evictions_bytes;
        break;
      case EvictionReason::invalidated:
        // Mutation-driven invalidation already accounts for the cause via
        // `rejected_oversize` (oversize put) or callers' own counters. The
        // eviction tallies stay reserved for budget-pressure evictions.
        break;
    }
  }

  void enforce_bounds() {
    while (options_.max_entries != 0 && entries_.size() > options_.max_entries) {
      auto front_key_it = index_.find(entries_.front().key);
      if (front_key_it == index_.end()) {
        entries_.pop_front();
        stats_.current_entries = entries_.size();
        continue;
      }
      drop_locked(front_key_it, EvictionReason::lru);
    }
    while (options_.max_bytes != 0 && stats_.current_bytes > options_.max_bytes && !entries_.empty()) {
      auto front_key_it = index_.find(entries_.front().key);
      if (front_key_it == index_.end()) {
        entries_.pop_front();
        stats_.current_entries = entries_.size();
        continue;
      }
      drop_locked(front_key_it, EvictionReason::bytes);
    }
  }

  Options options_{};
  [[no_unique_address]] ByteSizeOf size_of_{};
  std::list<Entry> entries_{};
  std::unordered_map<Key, ListIter, Hash, KeyEqual> index_{};
  Stats stats_{};
};

}  // namespace orangutan::core
