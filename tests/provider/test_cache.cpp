// tests/provider/test_cache.cpp — prompt-cache mapping validation.

#include <oran/provider.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

using orangutan::prompt::CacheSection;
using orangutan::prompt::RenderedPrompt;

CacheSection section(std::string id,
                     std::string content,
                     std::uint64_t content_hash,
                     std::uint32_t cache_version,
                     bool breakpoint = false) {
  return CacheSection{
      .id = std::move(id),
      .content = std::move(content),
      .content_hash = content_hash,
      .cache_version = cache_version,
      .is_breakpoint = breakpoint,
  };
}

RenderedPrompt rendered_prompt() {
  std::vector<CacheSection> sections{
      section("system_preamble", "system", 11, 1),
      section("tool_catalog", "tools", 22, 1),
      section("deferred_tools", "deferred", 33, 2),
      section("skills_catalog", "skills", 44, 1),
      section("memory_framing", "memory", 55, 1),
      section("per_agent_overlay", "overlay", 66, 3, true),
      section("conversation_tail", "tail changes", 77, 1),
  };

  std::size_t prefix_bytes = 0;
  for (std::size_t i = 0; i < sections.size() - 1; ++i) {
    prefix_bytes += sections[i].content.size();
  }

  return RenderedPrompt{
      .sections = std::move(sections),
      .prefix_hash = 0xC0FFEE,
      .prefix_bytes = prefix_bytes,
  };
}

}  // namespace

TEST_CASE("provider cache hints map only the rendered prompt prefix", "[unit][provider][cache]") {
  const auto rendered = rendered_prompt();

  const auto result = orangutan::provider::make_prompt_cache_hints(rendered);

  REQUIRE(result.has_value());
  REQUIRE(result->has_value());
  const auto& hints = **result;
  REQUIRE(hints.prefix_sections.size() == 6);
  REQUIRE(hints.breakpoint_section_index == 5);
  REQUIRE(hints.tail_section_index == 6);
  REQUIRE(hints.prefix_hash == rendered.prefix_hash);
  REQUIRE(hints.prefix_bytes == rendered.prefix_bytes);
  REQUIRE(hints.prefix_sections.front().id == "system_preamble");
  REQUIRE(hints.prefix_sections.front().content_hash == 11);
  REQUIRE(hints.prefix_sections[2].cache_version == 2);
  REQUIRE(std::ranges::none_of(hints.prefix_sections, [](const auto& key) { return key.id == "conversation_tail"; }));
}

TEST_CASE("provider cache hints can be disabled or skipped by prefix-size floor", "[unit][provider][cache]") {
  const auto rendered = rendered_prompt();

  const auto disabled = orangutan::provider::make_prompt_cache_hints(rendered, {.enabled = false});
  REQUIRE(disabled.has_value());
  REQUIRE_FALSE(disabled->has_value());

  const auto too_small =
      orangutan::provider::make_prompt_cache_hints(rendered, {.enabled = true, .min_prefix_bytes = 1'000'000});
  REQUIRE(too_small.has_value());
  REQUIRE_FALSE(too_small->has_value());
}

TEST_CASE("provider cache hints reject malformed rendered prompt boundaries", "[unit][provider][cache]") {
  SECTION("unexpected section count") {
    auto rendered = rendered_prompt();
    rendered.sections.pop_back();
    const auto result = orangutan::provider::make_prompt_cache_hints(rendered);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == orangutan::core::ErrorKind::invalid_argument);
  }

  SECTION("missing breakpoint") {
    auto rendered = rendered_prompt();
    rendered.sections[5].is_breakpoint = false;
    const auto result = orangutan::provider::make_prompt_cache_hints(rendered);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == orangutan::core::ErrorKind::invalid_argument);
  }

  SECTION("multiple breakpoints") {
    auto rendered = rendered_prompt();
    rendered.sections[4].is_breakpoint = true;
    const auto result = orangutan::provider::make_prompt_cache_hints(rendered);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == orangutan::core::ErrorKind::invalid_argument);
  }

  SECTION("breakpoint is not the final prefix section") {
    auto rendered = rendered_prompt();
    rendered.sections[5].is_breakpoint = false;
    rendered.sections[4].is_breakpoint = true;
    const auto result = orangutan::provider::make_prompt_cache_hints(rendered);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == orangutan::core::ErrorKind::invalid_argument);
  }

  SECTION("prefix byte count drift") {
    auto rendered = rendered_prompt();
    ++rendered.prefix_bytes;
    const auto result = orangutan::provider::make_prompt_cache_hints(rendered);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == orangutan::core::ErrorKind::invalid_argument);
  }
}
