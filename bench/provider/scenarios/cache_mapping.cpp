// bench/provider/scenarios/cache_mapping.cpp
//
// A-vs-B comparison: provider cache-hint mapping enabled vs. disabled route.

#include <nanobench.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <oran/provider.hpp>

namespace orangutan::bench {
namespace provider = orangutan::provider;
namespace prompt = orangutan::prompt;

namespace {

prompt::CacheSection section(std::string id,
                             std::string content,
                             std::uint64_t content_hash,
                             std::uint32_t cache_version,
                             bool breakpoint = false) {
  return prompt::CacheSection{
      .id = std::move(id),
      .content = std::move(content),
      .content_hash = content_hash,
      .cache_version = cache_version,
      .is_breakpoint = breakpoint,
  };
}

prompt::RenderedPrompt make_rendered_prompt() {
  std::vector<prompt::CacheSection> sections{
      section("system_preamble", std::string(1024, 's'), 11, 1),
      section("tool_catalog", std::string(4096, 't'), 22, 1),
      section("deferred_tools", std::string(512, 'd'), 33, 1),
      section("skills_catalog", std::string(512, 'k'), 44, 1),
      section("memory_framing", std::string(512, 'm'), 55, 1),
      section("per_agent_overlay", std::string(256, 'o'), 66, 1, true),
      section("conversation_tail", "user turn", 77, 1),
  };

  std::size_t prefix_bytes = 0;
  for (std::size_t i = 0; i < sections.size() - 1; ++i) {
    prefix_bytes += sections[i].content.size();
  }

  return prompt::RenderedPrompt{
      .sections = std::move(sections),
      .prefix_hash = 0x12345678,
      .prefix_bytes = prefix_bytes,
  };
}

std::size_t run_cache_mapping(bool enabled) {
  const auto rendered = make_rendered_prompt();
  auto hints = provider::make_prompt_cache_hints(rendered, {.enabled = enabled});
  if (!hints) {
    std::abort();
  }
  return hints->has_value() ? (*hints)->prefix_sections.size() : 0;
}

}  // namespace

void register_cache_mapping(ankerl::nanobench::Bench& bench) {
  bench.run("provider.cache_hints_enabled", [] {
    const auto mapped = run_cache_mapping(true);
    ankerl::nanobench::doNotOptimizeAway(mapped);
  });

  bench.run("provider.cache_hints_disabled", [] {
    const auto mapped = run_cache_mapping(false);
    ankerl::nanobench::doNotOptimizeAway(mapped);
  });
}

}  // namespace orangutan::bench
