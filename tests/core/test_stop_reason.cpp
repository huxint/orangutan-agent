// tests/core/test_stop_reason.cpp — `StopReason` mapping and formatter.

#include <format>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/stop_reason.hpp>

using orangutan::core::StopReason;
using orangutan::core::to_string_view;

TEST_CASE("to_string_view covers every StopReason enumerator", "[unit][core][stop_reason]") {
  REQUIRE(to_string_view(StopReason::end_turn) == "end_turn");
  REQUIRE(to_string_view(StopReason::max_tokens) == "max_tokens");
  REQUIRE(to_string_view(StopReason::tool_use) == "tool_use");
  REQUIRE(to_string_view(StopReason::stop_sequence) == "stop_sequence");
  REQUIRE(to_string_view(StopReason::cancelled) == "cancelled");
  REQUIRE(to_string_view(StopReason::error) == "error");
}

TEST_CASE("StopReason formats via std::format", "[unit][core][stop_reason]") {
  REQUIRE(std::format("{}", StopReason::end_turn) == "end_turn");
  REQUIRE(std::format("stop={}", StopReason::tool_use) == "stop=tool_use");
}

TEST_CASE("to_string_view falls back to 'unknown' for out-of-range StopReason", "[unit][core][stop_reason]") {
  const auto out_of_range = static_cast<StopReason>(99);
  REQUIRE(to_string_view(out_of_range) == "unknown");
}
