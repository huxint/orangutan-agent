// src/oran-provider/cache.cpp — validates prompt-cache hint boundaries.

#include <oran/provider/cache.hpp>

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <oran/core/error.hpp>

namespace orangutan::provider {
namespace {

constexpr std::size_t kExpectedPromptSections = 7;

[[nodiscard]] core::Error invalid_cache_prompt(std::string reason) {
  return core::Error::invalid_argument("provider prompt-cache mapping requires a valid rendered prompt")
      .with("reason", std::move(reason));
}

}  // namespace

core::Result<std::optional<PromptCacheHints>> make_prompt_cache_hints(const prompt::RenderedPrompt& rendered,
                                                                      PromptCacheOptions options) {
  if (!options.enabled) {
    return std::optional<PromptCacheHints>{};
  }

  if (rendered.sections.size() != kExpectedPromptSections) {
    return std::unexpected(
        invalid_cache_prompt("unexpected section count").with("sections", std::to_string(rendered.sections.size())));
  }

  std::size_t breakpoint_count = 0;
  std::size_t breakpoint_index = 0;
  for (std::size_t i = 0; i < rendered.sections.size(); ++i) {
    if (rendered.sections[i].is_breakpoint) {
      ++breakpoint_count;
      breakpoint_index = i;
    }
  }

  if (breakpoint_count != 1) {
    return std::unexpected(invalid_cache_prompt("expected exactly one cache breakpoint")
                               .with("breakpoints", std::to_string(breakpoint_count)));
  }

  const auto tail_index = rendered.sections.size() - 1;
  if (breakpoint_index + 1 != tail_index) {
    return std::unexpected(invalid_cache_prompt("breakpoint must be the final prefix section")
                               .with("breakpoint_index", std::to_string(breakpoint_index))
                               .with("tail_index", std::to_string(tail_index)));
  }

  std::size_t computed_prefix_bytes = 0;
  for (std::size_t i = 0; i <= breakpoint_index; ++i) {
    computed_prefix_bytes += rendered.sections[i].content.size();
  }

  if (computed_prefix_bytes != rendered.prefix_bytes) {
    return std::unexpected(invalid_cache_prompt("prefix byte count drift")
                               .with("expected", std::to_string(rendered.prefix_bytes))
                               .with("actual", std::to_string(computed_prefix_bytes)));
  }

  if (computed_prefix_bytes < options.min_prefix_bytes) {
    return std::optional<PromptCacheHints>{};
  }

  std::vector<PromptCacheSectionKey> prefix_sections;
  prefix_sections.reserve(breakpoint_index + 1);
  for (std::size_t i = 0; i <= breakpoint_index; ++i) {
    const auto& section = rendered.sections[i];
    prefix_sections.push_back(PromptCacheSectionKey{
        .id = section.id,
        .content_hash = section.content_hash,
        .cache_version = section.cache_version,
    });
  }

  return PromptCacheHints{
      .prefix_sections = std::move(prefix_sections),
      .breakpoint_section_index = breakpoint_index,
      .tail_section_index = tail_index,
      .prefix_hash = rendered.prefix_hash,
      .prefix_bytes = rendered.prefix_bytes,
  };
}

}  // namespace orangutan::provider
