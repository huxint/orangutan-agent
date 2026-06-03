// src/oran-prompt/builder.cpp — prompt section assembly.

#include <oran/prompt/builder.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/tool/catalog.hpp>

namespace orangutan::prompt {
namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;
constexpr std::string_view kHashSeparator = "\x1F";
constexpr auto kDefaultActiveTools = std::array<std::string_view, 8>{
    "file.read",
    "file.write",
    "file.edit",
    "file.search",
    "directory.list",
    "tool.search",
    "skill.invoke",
    "skill.deactivate",
};

[[nodiscard]] std::uint64_t hash_append(std::uint64_t hash, std::string_view bytes) noexcept {
  for (const char c : bytes) {
    hash ^= static_cast<unsigned char>(c);
    hash *= kFnvPrime;
  }
  return hash;
}

[[nodiscard]] std::uint64_t hash_append_u64(std::uint64_t hash, std::uint64_t value) noexcept {
  std::array<char, 8> bytes{};
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<char>((value >> (i * 8)) & 0xFFu);
  }
  return hash_append(hash, std::string_view{bytes.data(), bytes.size()});
}

[[nodiscard]] std::uint64_t stable_hash(std::string_view bytes) noexcept {
  return hash_append(kFnvOffset, bytes);
}

[[nodiscard]] bool catalog_contains(std::span<const core::ToolDef> catalog, std::string_view name) noexcept {
  return std::ranges::any_of(catalog, [name](const core::ToolDef& def) { return def.name == name; });
}

[[nodiscard]] bool is_active_tool(const config::PromptActiveToolsConfig& active_tools, std::string_view name) noexcept {
  if (active_tools.use_defaults) {
    return std::ranges::contains(kDefaultActiveTools, name);
  }
  return std::ranges::contains(active_tools.tool_names, name);
}

[[nodiscard]] bool is_promoted_tool(std::span<const std::string> promoted_tools, std::string_view name) noexcept {
  return std::ranges::contains(promoted_tools, name);
}

[[nodiscard]] core::Result<void> validate_explicit_active_tools(std::span<const core::ToolDef> catalog,
                                                                const config::PromptActiveToolsConfig& active_tools) {
  if (active_tools.use_defaults) {
    return {};
  }
  for (const auto& name : active_tools.tool_names) {
    if (!catalog_contains(catalog, name)) {
      return std::unexpected(core::Error::not_found("prompt active tool is not registered").with("tool", name));
    }
  }
  return {};
}

[[nodiscard]] std::vector<core::ToolDef> select_catalog(std::span<const core::ToolDef> catalog,
                                                        const config::PromptActiveToolsConfig& active_tools,
                                                        std::span<const std::string> promoted_tools) {
  std::vector<core::ToolDef> selected;
  selected.reserve(catalog.size());
  for (const auto& def : catalog) {
    auto copy = def;
    copy.deferred = !is_active_tool(active_tools, copy.name) && !is_promoted_tool(promoted_tools, copy.name);
    selected.push_back(std::move(copy));
  }
  return selected;
}

void append_content(std::string& out, const core::Content& content) {
  std::visit(
      [&](const auto& block) {
        using T = std::decay_t<decltype(block)>;
        if constexpr (std::same_as<T, core::TextContent>) {
          std::format_to(std::back_inserter(out), "text: {}\n", block.text);
        } else if constexpr (std::same_as<T, core::ThinkingContent>) {
          std::format_to(std::back_inserter(out), "thinking: {}\n", block.thinking);
          if (block.signature.has_value()) {
            std::format_to(std::back_inserter(out), "thinking_signature: {}\n", *block.signature);
          }
        } else if constexpr (std::same_as<T, core::ToolUseContent>) {
          std::format_to(std::back_inserter(out), "tool_use_id: {}\n", block.id);
          std::format_to(std::back_inserter(out), "tool_use_name: {}\n", block.name);
          std::format_to(std::back_inserter(out), "tool_use_input: {}\n", block.input_json);
        } else if constexpr (std::same_as<T, core::ToolResultContent>) {
          std::format_to(std::back_inserter(out), "tool_result_id: {}\n", block.tool_use_id);
          std::format_to(std::back_inserter(out), "tool_result_status: {}\n", block.is_error ? "error" : "ok");
          std::format_to(std::back_inserter(out), "tool_result_output: {}\n", block.output);
        }
      },
      content);
}

[[nodiscard]] std::string render_conversation_tail(std::span<const core::Message> messages) {
  std::string out;
  for (std::size_t i = 0; i < messages.size(); ++i) {
    if (i != 0) {
      out.push_back('\n');
    }
    std::format_to(std::back_inserter(out), "role: {}\n", core::enum_name(messages[i].role));
    for (const auto& block : messages[i].blocks) {
      append_content(out, block);
    }
  }
  return out;
}

[[nodiscard]] CacheSection
make_section(std::string id, std::string content, std::uint32_t cache_version, bool is_breakpoint = false) {
  const auto content_hash = stable_hash(content);
  return CacheSection{
      .id = std::move(id),
      .content = std::move(content),
      .content_hash = content_hash,
      .cache_version = cache_version,
      .is_breakpoint = is_breakpoint,
  };
}

[[nodiscard]] std::uint64_t hash_prefix(std::span<const CacheSection> sections) noexcept {
  auto hash = kFnvOffset;
  for (const auto& section : sections.first(6)) {
    hash = hash_append(hash, section.id);
    hash = hash_append(hash, kHashSeparator);
    hash = hash_append_u64(hash, section.cache_version);
    hash = hash_append(hash, kHashSeparator);
    hash = hash_append_u64(hash, section.content_hash);
    hash = hash_append(hash, kHashSeparator);
    hash = hash_append(hash, section.content);
    hash = hash_append(hash, kHashSeparator);
  }
  return hash;
}

[[nodiscard]] std::size_t prefix_bytes(std::span<const CacheSection> sections) noexcept {
  auto bytes = std::size_t{0};
  for (const auto& section : sections.first(6)) {
    bytes += section.content.size();
  }
  return bytes;
}

}  // namespace

class Builder::Impl {
public:
  explicit Impl(BuilderOptions options)
      : options_{options}, catalog_renderer_{tool::ToolCatalogRenderOptions{
                               .renderer_version = options_.tool_renderer_version,
                               .max_cached_blocks = options_.max_cached_tool_blocks,
                           }} {}

  [[nodiscard]] async::Awaitable<core::Result<RenderedPrompt>> build(BuilderInputs inputs) {
    auto validation = validate_explicit_active_tools(inputs.tool_catalog, inputs.active_tools);
    if (!validation) {
      co_return std::unexpected(std::move(validation.error()));
    }

    auto selected_catalog = select_catalog(inputs.tool_catalog, inputs.active_tools, inputs.promoted_tools);
    auto rendered_catalog = catalog_renderer_.render_catalog(selected_catalog);
    if (!rendered_catalog) {
      co_return std::unexpected(std::move(rendered_catalog.error()));
    }

    std::vector<CacheSection> sections;
    sections.reserve(7);
    sections.push_back(
        make_section("system_preamble", std::string{inputs.system_preamble}, options_.versions.system_preamble));
    sections.push_back(
        make_section("tool_catalog", std::move(rendered_catalog->active_text), options_.versions.tool_catalog));
    sections.push_back(
        make_section("deferred_tools", std::move(rendered_catalog->deferred_text), options_.versions.deferred_tools));
    sections.push_back(
        make_section("skills_catalog", std::string{inputs.skills_catalog}, options_.versions.skills_catalog));
    sections.push_back(
        make_section("memory_framing", std::string{inputs.memory_framing}, options_.versions.memory_framing));
    sections.push_back(make_section("per_agent_overlay",
                                    std::string{inputs.per_agent_overlay},
                                    options_.versions.per_agent_overlay,
                                    true));
    sections.push_back(make_section("conversation_tail",
                                    render_conversation_tail(inputs.conversation_tail),
                                    options_.versions.conversation_tail));

    const auto rendered_prefix_hash = hash_prefix(sections);
    const auto rendered_prefix_bytes = prefix_bytes(sections);

    co_return RenderedPrompt{
        .sections = std::move(sections),
        .prefix_hash = rendered_prefix_hash,
        .prefix_bytes = rendered_prefix_bytes,
    };
  }

  [[nodiscard]] const BuilderOptions& options() const noexcept {
    return options_;
  }

private:
  BuilderOptions options_{};
  tool::CatalogRenderer catalog_renderer_;
};

Builder::Builder(BuilderOptions options) : impl_{std::make_unique<Impl>(options)} {}

Builder::~Builder() = default;

Builder::Builder(Builder&&) noexcept = default;

Builder& Builder::operator=(Builder&&) noexcept = default;

async::Awaitable<core::Result<RenderedPrompt>> Builder::build(BuilderInputs inputs) {
  return impl_->build(inputs);
}

const BuilderOptions& Builder::options() const noexcept {
  return impl_->options();
}

}  // namespace orangutan::prompt
