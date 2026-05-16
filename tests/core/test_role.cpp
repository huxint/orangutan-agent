// tests/core/test_role.cpp — `Role` mapping and formatter.

#include <format>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/role.hpp>

using orangutan::core::parse_role;
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

TEST_CASE("parse_role round-trips every enumerator", "[unit][core][role]") {
  REQUIRE(parse_role("user") == std::optional{Role::user});
  REQUIRE(parse_role("assistant") == std::optional{Role::assistant});
  REQUIRE(parse_role("system") == std::optional{Role::system});
  REQUIRE(parse_role("tool") == std::optional{Role::tool});
}

TEST_CASE("parse_role rejects unknown spellings", "[unit][core][role]") {
  REQUIRE_FALSE(parse_role("").has_value());
  REQUIRE_FALSE(parse_role("User").has_value());
  REQUIRE_FALSE(parse_role("USER").has_value());
  REQUIRE_FALSE(parse_role("agent").has_value());
  REQUIRE_FALSE(parse_role("unknown").has_value());
  // `to_string_view`'s fallback string must not itself round-trip.
  REQUIRE_FALSE(parse_role(to_string_view(static_cast<Role>(99))).has_value());
}
