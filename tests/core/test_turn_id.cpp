// tests/core/test_turn_id.cpp — TurnId value-type coverage.

#include <cstddef>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/turn_id.hpp>

namespace core = orangutan::core;

TEST_CASE("TurnId is a 16-byte value and zero detection is explicit", "[unit][core][turn_id]") {
  core::TurnId id{};
  REQUIRE(id.size() == 16);
  REQUIRE(core::is_zero_turn_id(id));

  id[7] = std::byte{0x42};
  REQUIRE_FALSE(core::is_zero_turn_id(id));
}
