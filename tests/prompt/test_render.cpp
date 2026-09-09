#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/message.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/prompt.hpp>

namespace core = orangutan::core;
namespace prompt = orangutan::prompt;

namespace {

prompt::RenderInputs inputs_for(std::span<const core::ToolDef> tools) {
  return prompt::RenderInputs{
      .system_preamble = "system",
      .tools = tools,
      .skills_catalog = "skills",
      .memory_framing = "memory",
      .per_agent_overlay = "overlay",
  };
}

}  // namespace

TEST_CASE("Prompt fingerprints native tools without duplicating them in text", "[unit][prompt]") {
  const std::vector<core::ToolDef> tools{core::ToolDef{
      .name = "CustomLookup",
      .description = "Look up a topic in the host service.",
      .input_schema_json = R"({"type":"object","properties":{"topic_key":{"type":"string"}}})",
      .required_capabilities = {core::Capability::egress_http},
  }};
  const auto result = prompt::render(inputs_for(tools));

  constexpr auto ids =
      std::array{"system_preamble", "skills_catalog", "memory_framing", "per_agent_overlay", "conversation_tail"};
  REQUIRE(std::ranges::equal(result.sections, ids, {}, &prompt::CacheSection::id));
  REQUIRE(std::ranges::none_of(result.sections, [](const auto& section) {
    return section.content.contains("CustomLookup") || section.content.contains("topic_key") ||
           section.content.contains("host service") || section.content.contains("egress_http");
  }));
  REQUIRE(result.tool_catalog_bytes ==
          tools[0].name.size() + tools[0].description.size() + tools[0].input_schema_json.size());
  const auto text_bytes = std::ranges::fold_left(result.sections, std::size_t{0}, [](auto size, const auto& section) {
    return size + section.content.size();
  });
  REQUIRE(result.prefix_bytes == text_bytes + result.tool_catalog_bytes);
}

TEST_CASE("Prompt prefix remains stable across conversation tails", "[unit][prompt]") {
  const std::vector<core::ToolDef> tools{core::ToolDef::with_no_input("CustomLookup", "Find context")};
  const std::vector<core::Message> tail_a{core::Message::user_text("first")};
  const std::vector<core::Message> tail_b{core::Message::user_text("second")};
  auto inputs = inputs_for(tools);
  inputs.conversation_tail = tail_a;
  const auto first = prompt::render(inputs);
  inputs.conversation_tail = tail_b;
  const auto second = prompt::render(inputs);

  REQUIRE(first.prefix_hash == second.prefix_hash);
  REQUIRE(first.prefix_bytes == second.prefix_bytes);
  REQUIRE(first.tool_catalog_hash == second.tool_catalog_hash);
  REQUIRE(std::ranges::equal(std::span{first.sections}.first(4), std::span{second.sections}.first(4)));
  REQUIRE(first.sections.back().content != second.sections.back().content);
}

TEST_CASE("Native tool changes invalidate the prefix without changing text", "[unit][prompt]") {
  std::vector<core::ToolDef> tools{core::ToolDef::with_no_input("CustomLookup", "Find context"),
                                   core::ToolDef::with_no_input("MemoryRecall", "Read saved context")};
  const auto first = prompt::render(inputs_for(tools));

  SECTION("name") {
    tools[0].name = "CustomRead";
  }
  SECTION("description") {
    tools[0].description = "Find current context";
  }
  SECTION("schema") {
    tools[0].input_schema_json = R"({"type":"object","properties":{"topic":{"type":"string"}}})";
  }
  SECTION("selection") {
    tools.pop_back();
  }
  SECTION("native declaration order") {
    std::ranges::reverse(tools);
  }

  const auto second = prompt::render(inputs_for(tools));
  REQUIRE(first.sections == second.sections);
  REQUIRE(first.tool_catalog_hash != second.tool_catalog_hash);
  REQUIRE(first.prefix_hash != second.prefix_hash);
}

TEST_CASE("Native tool fingerprints preserve field boundaries", "[unit][prompt]") {
  std::vector<core::ToolDef> tools{core::ToolDef::with_no_input("ab", "c")};
  const auto first = prompt::render(inputs_for(tools));
  tools[0].name = "a";
  tools[0].description = "bc";
  const auto second = prompt::render(inputs_for(tools));
  REQUIRE(first.prefix_bytes == second.prefix_bytes);
  REQUIRE(first.prefix_hash != second.prefix_hash);
}

TEST_CASE("Dispatch capabilities do not enter model-visible cache identity", "[unit][prompt]") {
  std::vector<core::ToolDef> tools{core::ToolDef::with_no_input("CustomLookup", "Find context")};
  const auto first = prompt::render(inputs_for(tools));
  tools[0].required_capabilities = {core::Capability::read_memory};
  REQUIRE(prompt::render(inputs_for(tools)) == first);
}

TEST_CASE("Prompt versions invalidate cache identity without changing content", "[unit][prompt]") {
  const std::vector<core::ToolDef> tools{core::ToolDef::with_no_input("CustomLookup", "Find context")};
  auto versions = prompt::SectionVersions{};
  const auto first = prompt::render(inputs_for(tools), versions);

  SECTION("native tools") {
    ++versions.tool_catalog;
  }
  SECTION("stable text") {
    ++versions.memory_framing;
  }

  const auto second = prompt::render(inputs_for(tools), versions);
  REQUIRE(std::ranges::equal(first.sections,
                             second.sections,
                             {},
                             &prompt::CacheSection::content,
                             &prompt::CacheSection::content));
  REQUIRE(first.tool_catalog_hash == second.tool_catalog_hash);
  REQUIRE(first.prefix_bytes == second.prefix_bytes);
  REQUIRE(first.prefix_hash != second.prefix_hash);
}
