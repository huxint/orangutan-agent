// include/oran/provider/cache.hpp — prompt-cache mapping for provider adapters.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <oran/core/result.hpp>
#include <oran/prompt/builder.hpp>

namespace orangutan::provider {

struct PromptCacheSectionKey {
  std::string id;
  std::uint64_t content_hash{0};
  std::uint32_t cache_version{1};

  friend bool operator==(const PromptCacheSectionKey&, const PromptCacheSectionKey&) = default;
};

struct PromptCacheHints {
  std::vector<PromptCacheSectionKey> prefix_sections;
  std::size_t breakpoint_section_index{0};
  std::size_t tail_section_index{0};
  std::uint64_t prefix_hash{0};
  std::size_t prefix_bytes{0};

  friend bool operator==(const PromptCacheHints&, const PromptCacheHints&) = default;
};

struct PromptCacheOptions {
  bool enabled{true};
  std::size_t min_prefix_bytes{0};

  friend bool operator==(const PromptCacheOptions&, const PromptCacheOptions&) = default;
};

/// Build adapter-facing cache hints from `prompt::RenderedPrompt`.
///
/// The mapping validates the prompt-design invariant that exactly one cache
/// breakpoint sits on the final prefix section, immediately before the
/// conversation tail. When disabled or under the configured prefix-byte floor,
/// the result is `std::nullopt` so provider routes can silently skip cache
/// controls without mutating the prompt bytes.
[[nodiscard]] core::Result<std::optional<PromptCacheHints>>
make_prompt_cache_hints(const prompt::RenderedPrompt& rendered, PromptCacheOptions options = {});

}  // namespace orangutan::provider
