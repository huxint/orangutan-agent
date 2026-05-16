// src/oran-core/stop_reason.cpp — stable string mapping for `core::StopReason`.

#include <oran/core/stop_reason.hpp>

#include <array>
#include <cstddef>
#include <string_view>

namespace orangutan::core {

namespace {

constexpr std::array<std::string_view, 6> kStopReasonNames{
    "end_turn",
    "max_tokens",
    "tool_use",
    "stop_sequence",
    "cancelled",
    "error",
};

}  // namespace

std::string_view to_string_view(StopReason s) noexcept {
  const auto idx = static_cast<std::size_t>(s);
  if (idx < kStopReasonNames.size()) {
    return kStopReasonNames[idx];
  }
  return "unknown";
}

}  // namespace orangutan::core
