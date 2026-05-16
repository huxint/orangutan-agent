// tests/core/test_capability.cpp — `Capability` enumerator, parse, and formatter.

#include <algorithm>
#include <format>
#include <optional>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/capability.hpp>

using orangutan::core::Capability;
using orangutan::core::kAllCapabilities;
using orangutan::core::parse_capability;
using orangutan::core::to_string_view;

TEST_CASE("to_string_view covers every Capability enumerator", "[unit][core][capability]") {
  REQUIRE(to_string_view(Capability::read_file) == "read_file");
  REQUIRE(to_string_view(Capability::write_file) == "write_file");
  REQUIRE(to_string_view(Capability::edit_file) == "edit_file");
  REQUIRE(to_string_view(Capability::delete_path) == "delete_path");
  REQUIRE(to_string_view(Capability::egress_http) == "egress_http");
  REQUIRE(to_string_view(Capability::egress_websocket) == "egress_websocket");
  REQUIRE(to_string_view(Capability::spawn_subprocess) == "spawn_subprocess");
  REQUIRE(to_string_view(Capability::signal_subprocess) == "signal_subprocess");
  REQUIRE(to_string_view(Capability::read_memory) == "read_memory");
  REQUIRE(to_string_view(Capability::write_memory) == "write_memory");
  REQUIRE(to_string_view(Capability::spawn_agent) == "spawn_agent");
  REQUIRE(to_string_view(Capability::send_message_intra_team) == "send_message_intra_team");
  REQUIRE(to_string_view(Capability::send_message_inter_team) == "send_message_inter_team");
  REQUIRE(to_string_view(Capability::schedule_job) == "schedule_job");
  REQUIRE(to_string_view(Capability::modify_job) == "modify_job");
  REQUIRE(to_string_view(Capability::run_job_now) == "run_job_now");
  REQUIRE(to_string_view(Capability::invoke_skill) == "invoke_skill");
  REQUIRE(to_string_view(Capability::external_mcp) == "external_mcp");
  REQUIRE(to_string_view(Capability::runtime_loader) == "runtime_loader");
}

TEST_CASE("Capability formats via std::format", "[unit][core][capability]") {
  REQUIRE(std::format("{}", Capability::read_file) == "read_file");
  REQUIRE(std::format("[cap={}]", Capability::spawn_subprocess) == "[cap=spawn_subprocess]");
  REQUIRE(std::format("{}", Capability::runtime_loader) == "runtime_loader");
}

TEST_CASE("to_string_view falls back to 'unknown' for out-of-range Capability", "[unit][core][capability]") {
  const auto out_of_range = static_cast<Capability>(99);
  REQUIRE(to_string_view(out_of_range) == "unknown");
}

TEST_CASE("parse_capability round-trips every enumerator", "[unit][core][capability]") {
  for (const auto cap : kAllCapabilities()) {
    const auto parsed = parse_capability(to_string_view(cap));
    REQUIRE(parsed.has_value());
    REQUIRE(*parsed == cap);
  }
}

TEST_CASE("parse_capability rejects unknown spellings", "[unit][core][capability]") {
  REQUIRE_FALSE(parse_capability("").has_value());
  REQUIRE_FALSE(parse_capability("Read_File").has_value());
  REQUIRE_FALSE(parse_capability("read-file").has_value());
  REQUIRE_FALSE(parse_capability("readfile").has_value());
  REQUIRE_FALSE(parse_capability("network").has_value());
  // The `to_string_view` fallback sentinel must not round-trip.
  REQUIRE_FALSE(parse_capability(to_string_view(static_cast<Capability>(99))).has_value());
}

TEST_CASE("kAllCapabilities lists every enumerator exactly once", "[unit][core][capability]") {
  const auto all = kAllCapabilities();
  // The size matches the enumerator list and every entry round-trips through
  // the string mapping (so if a future edit forgets to update one of the two
  // arrays in capability.cpp, this case fails loudly).
  REQUIRE(all.size() == 19);
  for (const auto cap : all) {
    REQUIRE(to_string_view(cap) != "unknown");
  }
  // The view is in declaration order; check the endpoints explicitly so a
  // reordering shows up here rather than silently moving the bench fixture.
  REQUIRE(all.front() == Capability::read_file);
  REQUIRE(all.back() == Capability::runtime_loader);
  // Spellings are unique.
  std::vector<std::string_view> spellings;
  spellings.reserve(all.size());
  for (const auto cap : all) {
    spellings.push_back(to_string_view(cap));
  }
  std::ranges::sort(spellings);
  const auto duplicate = std::ranges::adjacent_find(spellings);
  REQUIRE(duplicate == spellings.end());
}
