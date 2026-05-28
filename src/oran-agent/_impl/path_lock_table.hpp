// src/oran-agent/_impl/path_lock_table.hpp — per-path read/write lock table.
//
// Private helper for `agent::ToolScheduler`. Slice 117 of
// `docs/exec-plans/active/2026-05-27-tool-scheduler-v1.md` lands the
// per-canonical-path read/write lock table required by
// spec 0012 AC3 / AC4 / AC10.
//
// Semantics:
//
//   - One `Entry` per lock key (canonical workspace-resolved absolute path).
//   - Shared locks may overlap; exclusive locks are mutually exclusive with
//     everything else.
//   - FIFO with writer-priority bypass: shared waiters at the front of the
//     queue may all wake when no writer is active, but a queued exclusive
//     blocks new shared acquirers from skipping the line.
//   - Idle entries (no readers, no writer, no waiters) are reaped on demand
//     after their idle age exceeds `Options::idle_ttl`. The TTL clock is the
//     caller-supplied `core::Time` so the table stays clock-agnostic.
//
// Concurrency: the table is single-strand by contract, matching
// `core::BoundedCache` and the agent loop's executor discipline. The scheduler
// drives every `acquire` / `release` / `reap` from the same `asio` executor.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include <asio/any_io_executor.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/async/channel.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>

namespace orangutan::agent::detail {

enum class PathLockMode : std::uint8_t {
  shared,
  exclusive,
};

class PathLockTable;

/// Move-only RAII handle that releases its lock on destruction. The guard does
/// not extend the lifetime of the owning `PathLockTable`; the scheduler owns
/// the table for as long as any spawned dispatch may hold a guard.
class PathLockGuard {
public:
  PathLockGuard() noexcept = default;
  PathLockGuard(PathLockTable* table, std::string path, PathLockMode mode) noexcept;
  PathLockGuard(const PathLockGuard&) = delete;
  PathLockGuard& operator=(const PathLockGuard&) = delete;
  PathLockGuard(PathLockGuard&& other) noexcept;
  PathLockGuard& operator=(PathLockGuard&& other) noexcept;
  ~PathLockGuard();

  [[nodiscard]] bool holds_lock() const noexcept {
    return held_;
  }

private:
  void reset() noexcept;

  PathLockTable* table_{nullptr};
  std::string path_{};
  PathLockMode mode_{PathLockMode::shared};
  bool held_{false};
};

struct PathLockTableOptions {
  /// Idle entries older than `idle_ttl` are removed on `reap`. `0` disables
  /// the TTL (entries live until manually erased).
  std::chrono::milliseconds idle_ttl{};
};

struct PathLockTableStats {
  /// Counters are monotonic over the table's lifetime.
  std::uint64_t shared_acquires{0};
  std::uint64_t exclusive_acquires{0};
  /// Acquires that had to wait for a holder or queued waiter.
  std::uint64_t contended_acquires{0};
  /// Acquire calls that were cancelled before the lock was granted.
  std::uint64_t cancelled_acquires{0};
  /// Entries removed by `reap`.
  std::uint64_t reaped_entries{0};
  std::size_t current_entries{0};
  std::size_t peak_entries{0};
};

class PathLockTable {
public:
  explicit PathLockTable(PathLockTableOptions options) noexcept;
  PathLockTable(const PathLockTable&) = delete;
  PathLockTable& operator=(const PathLockTable&) = delete;
  PathLockTable(PathLockTable&&) noexcept = default;
  PathLockTable& operator=(PathLockTable&&) noexcept = default;

  /// Acquire a lock keyed by `path` in the requested mode. Returns a guard on
  /// success, or `Error::cancelled` if the awaiting coroutine was cancelled
  /// while waiting. The path is moved in to avoid a copy on the hot path.
  [[nodiscard]] async::Awaitable<core::Result<PathLockGuard>>
  acquire(asio::any_io_executor exec, std::string path, PathLockMode mode, core::Time now);

  /// Drop idle entries whose idle age exceeds `Options::idle_ttl`. Returns the
  /// number of entries removed. Has no effect when `idle_ttl == 0ms`.
  std::size_t reap(core::Time now);

  [[nodiscard]] PathLockTableStats stats() const noexcept {
    return stats_;
  }

  /// Called by `PathLockGuard::~PathLockGuard()`. Not part of the public
  /// scheduler surface; exposed because the guard needs to call into it.
  void release(std::string_view path, PathLockMode mode);

private:
  struct Waiter {
    std::uint64_t id;
    PathLockMode mode;
    std::shared_ptr<async::Channel<std::monostate>> wake;
  };

  struct Entry {
    std::size_t readers{0};
    bool writer{false};
    std::deque<Waiter> waiters{};
    std::optional<core::Time> idle_since{};
  };

  struct TransparentStringHash {
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
      return std::hash<std::string_view>{}(value);
    }

    [[nodiscard]] std::size_t operator()(const std::string& value) const noexcept {
      return (*this)(std::string_view{value});
    }
  };

  /// Wake the appropriate next batch of waiters after a state-changing event.
  /// On wake, the entry's reader/writer counters are pre-incremented on behalf
  /// of each woken waiter, so a subsequent acquire that races against the wake
  /// sees the busy state.
  void wake_next_locked(Entry& entry);

  /// Update `idle_since` based on the current entry state.
  void update_idle_locked(Entry& entry, core::Time now) noexcept;

  PathLockTableOptions options_;
  std::unordered_map<std::string, Entry, TransparentStringHash, std::equal_to<>> entries_;
  PathLockTableStats stats_{};
  std::uint64_t next_waiter_id_{1};
};

}  // namespace orangutan::agent::detail
