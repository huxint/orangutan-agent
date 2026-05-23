// include/oran/prompt/promotion_state.hpp — bounded deferred-tool promotions.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <oran/core/result.hpp>
#include <oran/core/time.hpp>

namespace orangutan::prompt {

struct PromotionStateOptions {
  std::size_t max_promoted_tools{16};
  std::chrono::seconds ttl{std::chrono::hours{24}};

  friend bool operator==(const PromotionStateOptions&, const PromotionStateOptions&) = default;
};

struct PromotionStateStats {
  std::uint64_t promotions{0};
  std::uint64_t refreshes{0};
  std::uint64_t hits{0};
  std::uint64_t misses{0};
  std::uint64_t evictions_lru{0};
  std::uint64_t evictions_ttl{0};
  std::size_t current_entries{0};

  friend bool operator==(const PromotionStateStats&, const PromotionStateStats&) = default;
};

struct PromotionSnapshot {
  std::vector<std::string> tool_names;
  PromotionStateStats stats{};

  friend bool operator==(const PromotionSnapshot&, const PromotionSnapshot&) = default;
};

class PromotionState {
public:
  explicit PromotionState(PromotionStateOptions options = {});
  ~PromotionState();

  PromotionState(const PromotionState&) = delete;
  PromotionState& operator=(const PromotionState&) = delete;
  PromotionState(PromotionState&&) noexcept;
  PromotionState& operator=(PromotionState&&) noexcept;

  [[nodiscard]] core::Result<void> promote(std::string_view tool_name, core::Time now);
  [[nodiscard]] bool contains(std::string_view tool_name, core::Time now);
  [[nodiscard]] PromotionSnapshot snapshot(core::Time now);
  std::size_t reap(core::Time now);
  void clear();

  [[nodiscard]] const PromotionStateOptions& options() const noexcept;
  [[nodiscard]] PromotionStateStats stats() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace orangutan::prompt
