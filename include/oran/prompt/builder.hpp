// include/oran/prompt/builder.hpp — deterministic prompt section builder.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/config/config.hpp>
#include <oran/core/message.hpp>
#include <oran/core/result.hpp>
#include <oran/core/tool_def.hpp>

namespace orangutan::prompt {

struct SectionVersions {
  std::uint32_t system_preamble{1};
  std::uint32_t tool_catalog{1};
  std::uint32_t deferred_tools{1};
  std::uint32_t skills_catalog{1};
  std::uint32_t memory_framing{1};
  std::uint32_t per_agent_overlay{1};
  std::uint32_t conversation_tail{1};

  friend bool operator==(const SectionVersions&, const SectionVersions&) = default;
};

struct BuilderOptions {
  SectionVersions versions{};
  std::uint32_t tool_renderer_version{1};
  std::size_t max_cached_tool_blocks{256};

  friend bool operator==(const BuilderOptions&, const BuilderOptions&) = default;
};

struct BuilderInputs {
  std::string_view system_preamble{};
  std::span<const core::ToolDef> tool_catalog{};
  config::PromptActiveToolsConfig active_tools{};
  std::span<const std::string> promoted_tools{};
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
  std::uint64_t prefix_hash{0};
  std::size_t prefix_bytes{0};

  friend bool operator==(const RenderedPrompt&, const RenderedPrompt&) = default;
};

class Builder {
public:
  explicit Builder(BuilderOptions options = {});
  ~Builder();

  Builder(const Builder&) = delete;
  Builder& operator=(const Builder&) = delete;
  Builder(Builder&&) noexcept;
  Builder& operator=(Builder&&) noexcept;

  [[nodiscard]] async::Awaitable<core::Result<RenderedPrompt>> build(BuilderInputs inputs);

  [[nodiscard]] const BuilderOptions& options() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace orangutan::prompt
