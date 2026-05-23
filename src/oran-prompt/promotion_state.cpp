// src/oran-prompt/promotion_state.cpp — bounded deferred-tool promotion state.

#include <oran/prompt/promotion_state.hpp>

#include <algorithm>
#include <chrono>
#include <expected>
#include <iterator>
#include <list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <oran/core/error.hpp>

namespace orangutan::prompt {
namespace {

[[nodiscard]] bool ttl_expired(core::Time inserted_at, core::Time now, std::chrono::seconds ttl) noexcept {
  if (ttl == std::chrono::seconds{0}) {
    return false;
  }
  const auto age = now.to_system_time_point() - inserted_at.to_system_time_point();
  return age > ttl;
}

}  // namespace

class PromotionState::Impl {
public:
  explicit Impl(PromotionStateOptions options) : options_{options} {}

  [[nodiscard]] core::Result<void> promote(std::string_view tool_name, core::Time now) {
    if (tool_name.empty()) {
      return std::unexpected(core::Error::invalid_argument("promotion tool name is empty"));
    }
    reap(now);

    if (options_.max_promoted_tools == 0) {
      clear();
      return {};
    }

    if (const auto existing = index_.find(tool_name); existing != index_.end()) {
      existing->second->inserted_at = now;
      entries_.splice(entries_.end(), entries_, existing->second);
      ++stats_.refreshes;
      return {};
    }

    entries_.push_back(Entry{.tool_name = std::string{tool_name}, .inserted_at = now});
    index_.emplace(entries_.back().tool_name, std::prev(entries_.end()));
    ++stats_.promotions;
    stats_.current_entries = entries_.size();

    while (entries_.size() > options_.max_promoted_tools) {
      drop(index_.find(entries_.front().tool_name), EvictionReason::lru);
    }
    return {};
  }

  [[nodiscard]] bool contains(std::string_view tool_name, core::Time now) {
    reap(now);
    const auto it = index_.find(tool_name);
    if (it == index_.end()) {
      ++stats_.misses;
      return false;
    }
    entries_.splice(entries_.end(), entries_, it->second);
    ++stats_.hits;
    return true;
  }

  [[nodiscard]] PromotionSnapshot snapshot(core::Time now) {
    reap(now);
    std::vector<std::string> names;
    names.reserve(entries_.size());
    for (const auto& entry : entries_) {
      names.push_back(entry.tool_name);
    }
    std::ranges::sort(names);
    return PromotionSnapshot{.tool_names = std::move(names), .stats = stats_};
  }

  std::size_t reap(core::Time now) {
    if (options_.ttl == std::chrono::seconds{0}) {
      return 0;
    }
    auto evicted = std::size_t{0};
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (!ttl_expired(it->inserted_at, now, options_.ttl)) {
        ++it;
        continue;
      }
      const auto next = std::next(it);
      drop(index_.find(it->tool_name), EvictionReason::ttl);
      it = next;
      ++evicted;
    }
    return evicted;
  }

  void clear() {
    entries_.clear();
    index_.clear();
    stats_.current_entries = 0;
  }

  [[nodiscard]] const PromotionStateOptions& options() const noexcept {
    return options_;
  }

  [[nodiscard]] PromotionStateStats stats() const noexcept {
    return stats_;
  }

private:
  struct Entry {
    std::string tool_name;
    core::Time inserted_at{};
  };

  enum class EvictionReason {
    lru,
    ttl,
  };

  using EntryList = std::list<Entry>;
  using EntryIterator = EntryList::iterator;

  struct TransparentHash {
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
      return std::hash<std::string_view>{}(value);
    }
  };

  struct TransparentEqual {
    using is_transparent = void;

    [[nodiscard]] bool operator()(std::string_view lhs, std::string_view rhs) const noexcept {
      return lhs == rhs;
    }
  };

  void drop(std::unordered_map<std::string, EntryIterator, TransparentHash, TransparentEqual>::iterator it,
            EvictionReason reason) {
    if (it == index_.end()) {
      return;
    }
    entries_.erase(it->second);
    index_.erase(it);
    stats_.current_entries = entries_.size();
    if (reason == EvictionReason::lru) {
      ++stats_.evictions_lru;
    } else {
      ++stats_.evictions_ttl;
    }
  }

  PromotionStateOptions options_{};
  PromotionStateStats stats_{};
  EntryList entries_;
  std::unordered_map<std::string, EntryIterator, TransparentHash, TransparentEqual> index_;
};

PromotionState::PromotionState(PromotionStateOptions options) : impl_{std::make_unique<Impl>(options)} {}

PromotionState::~PromotionState() = default;

PromotionState::PromotionState(PromotionState&&) noexcept = default;

PromotionState& PromotionState::operator=(PromotionState&&) noexcept = default;

core::Result<void> PromotionState::promote(std::string_view tool_name, core::Time now) {
  return impl_->promote(tool_name, now);
}

bool PromotionState::contains(std::string_view tool_name, core::Time now) {
  return impl_->contains(tool_name, now);
}

PromotionSnapshot PromotionState::snapshot(core::Time now) {
  return impl_->snapshot(now);
}

std::size_t PromotionState::reap(core::Time now) {
  return impl_->reap(now);
}

void PromotionState::clear() {
  impl_->clear();
}

const PromotionStateOptions& PromotionState::options() const noexcept {
  return impl_->options();
}

PromotionStateStats PromotionState::stats() const noexcept {
  return impl_->stats();
}

}  // namespace orangutan::prompt
