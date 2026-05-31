// tests/agent/test_system_preamble.cpp - stable system preamble coverage.

#include <oran/agent.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace agent = orangutan::agent;

TEST_CASE("default system preamble is stable and scoped to section one", "[unit][agent][prompt]") {
  const auto first = agent::default_system_preamble();
  const auto second = agent::default_system_preamble();

  REQUIRE(first == second);
  REQUIRE(first.section_text.contains("You are Orangutan"));
  REQUIRE(first.section_text.contains("Operating principles:"));
  REQUIRE(first.section_text.contains("Response contract:"));
  REQUIRE(first.section_text.contains("Use tools for effects"));
  REQUIRE_FALSE(first.section_text.contains("Tool:"));
  REQUIRE_FALSE(first.section_text.contains("memory"));
  REQUIRE_FALSE(first.section_text.contains("skill"));
  REQUIRE_FALSE(first.section_text.contains("conversation"));
  REQUIRE_FALSE(first.section_text.contains("today"));
  REQUIRE_FALSE(first.section_text.contains("request id"));
  REQUIRE_FALSE(first.section_text.contains("trace id"));
}

TEST_CASE("system preamble owner renders stable bytes and counts calls", "[unit][agent][prompt]") {
  auto owner = agent::SystemPreambleOwner{};

  const auto first = std::string{owner.render_once()};
  const auto second = std::string{owner.render_once()};

  REQUIRE(first == second);
  REQUIRE(first == agent::default_system_preamble().section_text);
  REQUIRE(owner.stats().renders == 2);
}

TEST_CASE("system preamble owner accepts explicit override without resetting stats", "[unit][agent][prompt]") {
  auto owner = agent::SystemPreambleOwner{};
  REQUIRE(std::string{owner.render_once()}.contains("You are Orangutan"));

  owner.replace(agent::SystemPreamble{.section_text = "system: test override"});

  REQUIRE(std::string{owner.render_once()} == "system: test override");
  REQUIRE(owner.preamble().section_text == "system: test override");
  REQUIRE(owner.stats().renders == 2);

  owner.clear();

  REQUIRE(owner.render_once().empty());
  REQUIRE(owner.stats().renders == 3);
}
