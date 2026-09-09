#include <oran/prompt/render.hpp>

#include <array>
#include <concepts>
#include <format>
#include <iterator>
#include <type_traits>
#include <utility>
#include <variant>

#include <oran/core/enum_names.hpp>

namespace orangutan::prompt {
namespace {

constexpr std::uint64_t FNV_OFFSET = 14695981039346656037ull;
constexpr std::uint64_t FNV_PRIME = 1099511628211ull;

[[nodiscard]] std::uint64_t hash_append(std::uint64_t hash, std::string_view bytes) noexcept {
  for (const char c : bytes) {
    hash ^= static_cast<unsigned char>(c);
    hash *= FNV_PRIME;
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

[[nodiscard]] std::uint64_t hash_field(std::uint64_t hash, std::string_view bytes) noexcept {
  return hash_append(hash_append_u64(hash, bytes.size()), bytes);
}

[[nodiscard]] std::uint64_t hash_tools(std::span<const core::ToolDef> tools) noexcept {
  auto hash = hash_append_u64(FNV_OFFSET, tools.size());
  for (const auto& def : tools) {
    hash = hash_field(hash, def.name);
    hash = hash_field(hash, def.description);
    hash = hash_field(hash, def.input_schema_json);
  }
  return hash;
}

void append_content(std::string& out, const core::Content& content) {
  std::visit(
      [&out](const auto& block) {
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
make_section(std::string id, std::string content, std::uint32_t cache_version) {
  const auto content_hash = hash_append(FNV_OFFSET, content);
  return CacheSection{
      .id = std::move(id),
      .content = std::move(content),
      .content_hash = content_hash,
      .cache_version = cache_version,
  };
}

}  // namespace

RenderedPrompt render(RenderInputs inputs, SectionVersions versions) {
  RenderedPrompt rendered;
  rendered.tool_catalog_hash = hash_tools(inputs.tools);
  for (const auto& def : inputs.tools) {
    rendered.tool_catalog_bytes += def.name.size() + def.description.size() + def.input_schema_json.size();
  }

  auto& sections = rendered.sections;
  sections.reserve(5);
  sections.push_back(make_section("system_preamble", std::string{inputs.system_preamble}, versions.system_preamble));
  sections.push_back(make_section("skills_catalog", std::string{inputs.skills_catalog}, versions.skills_catalog));
  sections.push_back(make_section("memory_framing", std::string{inputs.memory_framing}, versions.memory_framing));
  sections.push_back(make_section("per_agent_overlay", std::string{inputs.per_agent_overlay}, versions.per_agent_overlay));
  sections.push_back(make_section("conversation_tail",
                                  render_conversation_tail(inputs.conversation_tail),
                                  versions.conversation_tail));

  auto hash = hash_field(FNV_OFFSET, "native_tools");
  hash = hash_append_u64(hash, versions.tool_catalog);
  hash = hash_append_u64(hash, rendered.tool_catalog_hash);
  rendered.prefix_bytes = rendered.tool_catalog_bytes;
  for (const auto& section : std::span{sections}.first(sections.size() - 1)) {
    hash = hash_field(hash, section.id);
    hash = hash_append_u64(hash, section.cache_version);
    hash = hash_field(hash, section.content);
    rendered.prefix_bytes += section.content.size();
  }
  rendered.prefix_hash = hash;
  return rendered;
}

}  // namespace orangutan::prompt
