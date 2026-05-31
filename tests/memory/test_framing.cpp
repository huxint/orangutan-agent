// tests/memory/test_framing.cpp — prompt memory framing owner coverage.

#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <oran/memory.hpp>

namespace memory = orangutan::memory;

TEST_CASE("FramingOwner renders stable section bytes and records render count", "[unit][memory][framing]") {
  memory::FramingOwner owner{memory::Framing{.section_text = "memory: stable"}};

  const auto first = owner.render_once();
  const auto second = owner.render_once();

  REQUIRE(first == std::string_view{"memory: stable"});
  REQUIRE(second == std::string_view{"memory: stable"});
  REQUIRE(owner.stats().renders == 2);
}

TEST_CASE("FramingOwner permits an empty memory section", "[unit][memory][framing]") {
  memory::FramingOwner owner;

  const auto rendered = owner.render_once();

  REQUIRE(rendered.empty());
  REQUIRE(owner.stats().renders == 1);
}

TEST_CASE("FramingOwner replaces and clears section state without resetting stats", "[unit][memory][framing]") {
  memory::FramingOwner owner{memory::Framing{.section_text = "memory: old"}};
  REQUIRE(owner.render_once() == std::string_view{"memory: old"});

  owner.replace(memory::Framing{.section_text = "memory: new"});
  REQUIRE(owner.render_once() == std::string_view{"memory: new"});

  owner.clear();
  REQUIRE(owner.render_once().empty());
  REQUIRE(owner.stats().renders == 3);
}
