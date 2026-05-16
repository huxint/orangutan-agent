// include/oran/core/stop_reason.hpp — terminal cause of a model turn.

#pragma once

#include <cstdint>
#include <format>
#include <string_view>

namespace orangutan::core {

enum class StopReason : std::uint8_t {
  end_turn,       ///< Model decided it was done.
  max_tokens,     ///< Token budget exhausted.
  tool_use,       ///< Model wants a tool call before continuing.
  stop_sequence,  ///< Configured stop string matched.
  cancelled,      ///< Execution layer (or user) cancelled.
  error,          ///< Protocol/upstream failure surfaced as a stop reason.
};

[[nodiscard]] std::string_view to_string_view(StopReason) noexcept;

}  // namespace orangutan::core

template <>
struct std::formatter<orangutan::core::StopReason> : std::formatter<std::string_view> {
  template <class FormatContext>
  auto format(orangutan::core::StopReason s, FormatContext& ctx) const {
    return std::formatter<std::string_view>::format(orangutan::core::to_string_view(s), ctx);
  }
};
