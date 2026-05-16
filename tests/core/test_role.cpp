// tests/core/test_role.cpp — `Role` mapping and formatter.

#include <format>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/role.hpp>

using orangutan::core::Role;
using orangutan::core::to_string_view;

TEST_CASE("to_string_view covers every Role enumerator", "[unit][core][role]") {
  REQUIRE(to_string_view(Role::user) == "user");
  REQUIRE(to_string_view(Role::assistant) == "assistant");
  REQUIRE(to_string_view(Role::system) == "system");
  REQUIRE(to_string_view(Role::tool) == "tool");
}

TEST_CASE("Role formats via std::format", "[unit][core][role]") {
  REQUIRE(std::format("{}", Role::user) == "user");
  REQUIRE(std::format("{}", Role::assistant) == "assistant");
  REQUIRE(std::format("[role={}]", Role::tool) == "[role=tool]");
}

TEST_CASE("to_string_view falls back to 'unknown' for out-of-range Role", "[unit][core][role]") {
  const auto out_of_range = static_cast<Role>(99);
  REQUIRE(to_string_view(out_of_range) == "unknown");
}
