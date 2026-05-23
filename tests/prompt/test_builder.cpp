// tests/prompt/test_builder.cpp — prompt builder coverage.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/config.hpp>
#include <oran/core/error.hpp>
#include <oran/core/message.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/prompt.hpp>

#include "../test-helpers/run_async.hpp"

namespace config = orangutan::config;
namespace core = orangutan::core;
namespace prompt = orangutan::prompt;
namespace test = orangutan::tests;

namespace {

core::Time at_seconds(int seconds) {
  return core::Time{core::Time::time_point{std::chrono::seconds{seconds}}};
}

core::ToolDef tool_def(std::string name, std::string description, bool deferred = false) {
  return core::ToolDef{
      .name = std::move(name),
      .description = std::move(description),
      .input_schema_json = R"({"type":"object","properties":{},"additionalProperties":false})",
      .required_capabilities = {},
      .deferred = deferred,
      .category = "test",
  };
}

const prompt::CacheSection& section(const prompt::RenderedPrompt& rendered, std::string_view id) {
  const auto it = std::ranges::find(rendered.sections, id, &prompt::CacheSection::id);
  REQUIRE(it != rendered.sections.end());
  return *it;
}

prompt::BuilderInputs inputs_for(std::span<const core::ToolDef> catalog,
                                 config::PromptActiveToolsConfig active_tools = {}) {
  return prompt::BuilderInputs{
      .system_preamble = "system",
      .tool_catalog = catalog,
      .active_tools = std::move(active_tools),
      .skills_catalog = "skills",
      .memory_framing = "memory",
      .per_agent_overlay = "overlay",
      .conversation_tail = {},
  };
}

}  // namespace

TEST_CASE("Builder renders the default active tool set and a deferred index", "[unit][prompt]") {
  test::run_async([](asio::io_context&) -> asio::awaitable<void> {
    const std::vector<core::ToolDef> catalog{
        tool_def("memory.recall", "Recall memory", true),
        tool_def("file.write", "Write a file"),
        tool_def("tool.search", "Search tools"),
        tool_def("file.read", "Read a file"),
    };

    prompt::Builder builder;
    auto result = co_await builder.build(inputs_for(catalog));

    REQUIRE(result.has_value());
    REQUIRE(result->sections.size() == 7);
    constexpr auto kExpectedIds = std::array{
        "system_preamble",
        "tool_catalog",
        "deferred_tools",
        "skills_catalog",
        "memory_framing",
        "per_agent_overlay",
        "conversation_tail",
    };
    for (std::size_t i = 0; i < kExpectedIds.size(); ++i) {
      REQUIRE(result->sections[i].id == kExpectedIds[i]);
    }
    auto expected_prefix_bytes = std::size_t{0};
    for (std::size_t i = 0; i < 6; ++i) {
      expected_prefix_bytes += result->sections[i].content.size();
    }
    REQUIRE(result->prefix_bytes == expected_prefix_bytes);
    REQUIRE(section(*result, "tool_catalog").content.find("Tool: file.read") <
            section(*result, "tool_catalog").content.find("Tool: file.write"));
    REQUIRE(section(*result, "tool_catalog").content.contains("Tool: tool.search"));
    REQUIRE_FALSE(section(*result, "tool_catalog").content.contains("memory.recall"));
    REQUIRE(section(*result, "deferred_tools").content == "memory.recall - Recall memory");
    REQUIRE(section(*result, "per_agent_overlay").is_breakpoint);
    REQUIRE(std::ranges::count(result->sections, true, &prompt::CacheSection::is_breakpoint) == 1);
  });
}

TEST_CASE("Builder uses explicit active tools and moves all others to the deferred index", "[unit][prompt]") {
  test::run_async([](asio::io_context&) -> asio::awaitable<void> {
    const std::vector<core::ToolDef> catalog{
        tool_def("file.write", "Write a file"),
        tool_def("tool.search", "Search tools"),
        tool_def("file.read", "Read a file"),
    };
    auto active_tools = config::PromptActiveToolsConfig{
        .use_defaults = false,
        .tool_names = {"file.read", "tool.search"},
    };

    prompt::Builder builder;
    auto result = co_await builder.build(inputs_for(catalog, std::move(active_tools)));

    REQUIRE(result.has_value());
    REQUIRE(section(*result, "tool_catalog").content.contains("Tool: file.read"));
    REQUIRE(section(*result, "tool_catalog").content.contains("Tool: tool.search"));
    REQUIRE_FALSE(section(*result, "tool_catalog").content.contains("Tool: file.write"));
    REQUIRE(section(*result, "deferred_tools").content == "file.write - Write a file");
  });
}

TEST_CASE("Builder can explicitly promote a deferred tool into the active catalog", "[unit][prompt]") {
  test::run_async([](asio::io_context&) -> asio::awaitable<void> {
    const std::vector<core::ToolDef> catalog{
        tool_def("memory.recall", "Recall memory", true),
        tool_def("file.read", "Read a file"),
    };
    auto active_tools = config::PromptActiveToolsConfig{
        .use_defaults = false,
        .tool_names = {"memory.recall"},
    };

    prompt::Builder builder;
    auto result = co_await builder.build(inputs_for(catalog, std::move(active_tools)));

    REQUIRE(result.has_value());
    REQUIRE(section(*result, "tool_catalog").content.contains("Tool: memory.recall"));
    REQUIRE_FALSE(section(*result, "tool_catalog").content.contains("Tool: file.read"));
    REQUIRE(section(*result, "deferred_tools").content == "file.read - Read a file");
  });
}

TEST_CASE("Builder applies a promotion snapshot to the next active catalog", "[unit][prompt]") {
  test::run_async([](asio::io_context&) -> asio::awaitable<void> {
    const std::vector<core::ToolDef> catalog{
        tool_def("memory.recall", "Recall memory", true),
        tool_def("agent.spawn", "Spawn an agent", true),
        tool_def("file.read", "Read a file"),
    };

    prompt::PromotionState promotions;
    REQUIRE(promotions.promote("memory.recall", at_seconds(1)).has_value());
    auto snapshot = promotions.snapshot(at_seconds(2));
    auto inputs = inputs_for(catalog);
    inputs.promoted_tools = snapshot.tool_names;

    prompt::Builder builder;
    auto result = co_await builder.build(inputs);

    REQUIRE(result.has_value());
    REQUIRE(section(*result, "tool_catalog").content.contains("Tool: memory.recall"));
    REQUIRE(section(*result, "tool_catalog").content.contains("Tool: file.read"));
    REQUIRE_FALSE(section(*result, "tool_catalog").content.contains("Tool: agent.spawn"));
    REQUIRE(section(*result, "deferred_tools").content == "agent.spawn - Spawn an agent");
  });
}

TEST_CASE("Builder reports missing explicit active tools", "[unit][prompt]") {
  test::run_async([](asio::io_context&) -> asio::awaitable<void> {
    const std::vector<core::ToolDef> catalog{
        tool_def("file.read", "Read a file"),
    };
    auto active_tools = config::PromptActiveToolsConfig{
        .use_defaults = false,
        .tool_names = {"tool.missing"},
    };

    prompt::Builder builder;
    auto result = co_await builder.build(inputs_for(catalog, std::move(active_tools)));

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::not_found);
    REQUIRE(std::ranges::any_of(result.error().context(), [](const auto& entry) {
      return entry.first == "tool" && entry.second == "tool.missing";
    }));
  });
}

TEST_CASE("PromotionState bounds promotions by LRU and returns a sorted snapshot", "[unit][prompt]") {
  auto state = prompt::PromotionState{prompt::PromotionStateOptions{
      .max_promoted_tools = 2,
      .ttl = std::chrono::hours{24},
  }};

  REQUIRE(state.promote("zeta.tool", at_seconds(1)).has_value());
  REQUIRE(state.promote("alpha.tool", at_seconds(2)).has_value());
  REQUIRE(state.contains("zeta.tool", at_seconds(3)));
  REQUIRE(state.promote("beta.tool", at_seconds(4)).has_value());

  auto snapshot = state.snapshot(at_seconds(5));
  REQUIRE(snapshot.tool_names == std::vector<std::string>{"beta.tool", "zeta.tool"});
  REQUIRE(snapshot.stats.promotions == 3);
  REQUIRE(snapshot.stats.hits == 1);
  REQUIRE(snapshot.stats.evictions_lru == 1);
  REQUIRE(snapshot.stats.current_entries == 2);
}

TEST_CASE("PromotionState reaps expired promotions", "[unit][prompt]") {
  auto state = prompt::PromotionState{prompt::PromotionStateOptions{
      .max_promoted_tools = 16,
      .ttl = std::chrono::seconds{10},
  }};

  REQUIRE(state.promote("memory.recall", at_seconds(1)).has_value());
  REQUIRE(state.contains("memory.recall", at_seconds(10)));
  REQUIRE_FALSE(state.contains("memory.recall", at_seconds(12)));
  REQUIRE(state.stats().evictions_ttl == 1);
  REQUIRE(state.stats().current_entries == 0);
}

TEST_CASE("PromotionState rejects empty tool names", "[unit][prompt]") {
  prompt::PromotionState state;
  auto result = state.promote("", at_seconds(1));

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
}

TEST_CASE("Builder keeps the cached prefix stable across conversation tails", "[unit][prompt]") {
  test::run_async([](asio::io_context&) -> asio::awaitable<void> {
    const std::vector<core::ToolDef> catalog{
        tool_def("file.read", "Read a file"),
        tool_def("tool.search", "Search tools"),
    };
    const std::vector<core::Message> tail_a{core::Message::user_text("first")};
    const std::vector<core::Message> tail_b{core::Message::user_text("second")};

    prompt::Builder builder;
    auto inputs_a = inputs_for(catalog);
    inputs_a.conversation_tail = tail_a;
    auto inputs_b = inputs_for(catalog);
    inputs_b.conversation_tail = tail_b;

    auto first = co_await builder.build(inputs_a);
    auto second = co_await builder.build(inputs_b);

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(first->prefix_hash == second->prefix_hash);
    REQUIRE(first->prefix_bytes == second->prefix_bytes);
    for (std::size_t i = 0; i < 6; ++i) {
      REQUIRE(first->sections[i] == second->sections[i]);
    }
    REQUIRE(first->sections[6].content != second->sections[6].content);
  });
}

TEST_CASE("Builder cache versions invalidate the prefix hash without changing content", "[unit][prompt]") {
  test::run_async([](asio::io_context&) -> asio::awaitable<void> {
    const std::vector<core::ToolDef> catalog{
        tool_def("file.read", "Read a file"),
    };
    auto options = prompt::BuilderOptions{};
    options.versions.tool_catalog = 2;

    prompt::Builder v1;
    prompt::Builder v2{options};
    auto first = co_await v1.build(inputs_for(catalog));
    auto second = co_await v2.build(inputs_for(catalog));

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(section(*first, "tool_catalog").content == section(*second, "tool_catalog").content);
    REQUIRE(section(*first, "tool_catalog").content_hash == section(*second, "tool_catalog").content_hash);
    REQUIRE(section(*first, "tool_catalog").cache_version == 1);
    REQUIRE(section(*second, "tool_catalog").cache_version == 2);
    REQUIRE(first->prefix_hash != second->prefix_hash);
  });
}
