// include/oran/agent/session_state.hpp — per-session agent state.

#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include <oran/core/result.hpp>
#include <oran/core/time.hpp>
#include <oran/prompt/promotion_state.hpp>
#include <oran/tool/output.hpp>

namespace orangutan::agent {

struct ToolPromotionReport {
  bool observed_tool_search{false};
  std::uint64_t matches_seen{0};
  std::uint64_t promoted{0};
  std::uint64_t skipped_non_deferred{0};

  friend bool operator==(const ToolPromotionReport&, const ToolPromotionReport&) = default;
};

class SessionState {
public:
  explicit SessionState(prompt::PromotionStateOptions promotion_options = {});
  ~SessionState();

  SessionState(const SessionState&) = delete;
  SessionState& operator=(const SessionState&) = delete;
  SessionState(SessionState&&) noexcept;
  SessionState& operator=(SessionState&&) noexcept;

  [[nodiscard]] core::Result<ToolPromotionReport>
  observe_tool_output(std::string_view tool_name, const tool::Output& output, core::Time now);

  [[nodiscard]] prompt::PromotionSnapshot promotion_snapshot(core::Time now);
  [[nodiscard]] prompt::PromotionStateStats promotion_stats() const noexcept;
  [[nodiscard]] const prompt::PromotionStateOptions& promotion_options() const noexcept;
  void clear_promotions();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace orangutan::agent
