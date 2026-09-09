#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <oran/core/message.hpp>
#include <oran/core/tool_def.hpp>

namespace orangutan::prompt {

struct SectionVersions {
  std::uint32_t system_preamble{3};
  std::uint32_t tool_catalog{5};
  std::uint32_t skills_catalog{1};
  std::uint32_t memory_framing{2};
  std::uint32_t per_agent_overlay{1};
  std::uint32_t conversation_tail{1};

  friend bool operator==(const SectionVersions&, const SectionVersions&) = default;
};

struct RenderInputs {
  std::string_view system_preamble{};
  /// The same ordered definitions sent through the provider's native tools.
  /// Only name, description and schema contribute to the prefix fingerprint.
  std::span<const core::ToolDef> tools{};
  std::string_view skills_catalog{};
  std::string_view memory_framing{};
  std::string_view per_agent_overlay{};
  std::span<const core::Message> conversation_tail{};
};

struct CacheSection {
  std::string id;
  std::string content;
  std::uint64_t content_hash{0};
  std::uint32_t cache_version{1};
  bool is_breakpoint{false};

  friend bool operator==(const CacheSection&, const CacheSection&) = default;
};

struct RenderedPrompt {
  std::vector<CacheSection> sections;
  std::uint64_t tool_catalog_hash{0};
  /// Native name/description/schema bytes, excluding protocol framing.
  std::size_t tool_catalog_bytes{0};
  /// Includes native tools, stable text sections and their cache versions.
  std::uint64_t prefix_hash{0};
  /// Stable text bytes plus tool_catalog_bytes; not a wire-size estimate.
  std::size_t prefix_bytes{0};

  friend bool operator==(const RenderedPrompt&, const RenderedPrompt&) = default;
};

/// Render owned prompt text and cache identity from explicit values. Native
/// tool definitions affect the fingerprint without being copied into text.
[[nodiscard]] RenderedPrompt render(RenderInputs inputs, SectionVersions versions = {});

}  // namespace orangutan::prompt
