// tests/agent/test_session_state.cpp — agent session-state coverage.

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/agent.hpp>
#include <oran/config.hpp>
#include <oran/core/error.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/prompt.hpp>
#include <oran/tool.hpp>

#include "../test-helpers/run_async.hpp"

namespace agent = orangutan::agent;
namespace config = orangutan::config;
namespace core = orangutan::core;
namespace prompt = orangutan::prompt;
namespace test = orangutan::tests;
namespace tool = orangutan::tool;

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

tool::Output tool_search_output(std::string data_json) {
  return tool::Output{
      .text = "tool.search: matches",
      .data_json = std::move(data_json),
  };
}

}  // namespace

TEST_CASE("SessionState promotes deferred tool.search matches into the next prompt snapshot", "[unit][agent]") {
  test::run_async([](asio::io_context&) -> asio::awaitable<void> {
    agent::SessionState session;
    auto observed = session.observe_tool_output(
        tool::kToolSearchName,
        tool_search_output(
            R"({"kind":"tool_search","matches":[{"name":"memory.recall","deferred":true},{"name":"file.read","deferred":false}]})"),
        at_seconds(1));

    REQUIRE(observed.has_value());
    REQUIRE(observed->observed_tool_search);
    REQUIRE(observed->matches_seen == 2);
    REQUIRE(observed->promoted == 1);
    REQUIRE(observed->skipped_non_deferred == 1);

    auto snapshot = session.promotion_snapshot(at_seconds(2));
    REQUIRE(snapshot.tool_names == std::vector<std::string>{"memory.recall"});

    const std::vector<core::ToolDef> catalog{
        tool_def("memory.recall", "Recall memory", true),
        tool_def("file.read", "Read a file"),
    };
    prompt::Builder builder;
    auto result = co_await builder.build(prompt::BuilderInputs{
        .system_preamble = "system",
        .tool_catalog = catalog,
        .active_tools = config::PromptActiveToolsConfig{},
        .promoted_tools = snapshot.tool_names,
        .skills_catalog = "",
        .memory_framing = "",
        .per_agent_overlay = "",
        .conversation_tail = {},
    });

    REQUIRE(result.has_value());
    REQUIRE(section(*result, "tool_catalog").content.contains("Tool: memory.recall"));
    REQUIRE_FALSE(section(*result, "deferred_tools").content.contains("memory.recall"));
  });
}

TEST_CASE("SessionState ignores non-search and failed search outputs", "[unit][agent]") {
  agent::SessionState session;

  auto non_search = session.observe_tool_output(
      "file.read",
      tool_search_output(R"({"kind":"tool_search","matches":[{"name":"memory.recall","deferred":true}]})"),
      at_seconds(1));
  REQUIRE(non_search.has_value());
  REQUIRE_FALSE(non_search->observed_tool_search);
  REQUIRE(session.promotion_snapshot(at_seconds(2)).tool_names.empty());

  auto failed =
      session.observe_tool_output(tool::kToolSearchName, tool::Output::error("tool.search failed"), at_seconds(3));
  REQUIRE(failed.has_value());
  REQUIRE(failed->observed_tool_search);
  REQUIRE(failed->matches_seen == 0);
  REQUIRE(failed->promoted == 0);
  REQUIRE(session.promotion_snapshot(at_seconds(4)).tool_names.empty());
}

TEST_CASE("SessionState rejects malformed successful tool.search data", "[unit][agent]") {
  agent::SessionState session;
  auto malformed = session.observe_tool_output(tool::kToolSearchName,
                                               tool_search_output(R"({"kind":"tool_search","matches":[{"name":""}]})"),
                                               at_seconds(1));

  REQUIRE_FALSE(malformed.has_value());
  REQUIRE(malformed.error().kind() == core::ErrorKind::invalid_argument);
  REQUIRE(session.promotion_snapshot(at_seconds(2)).tool_names.empty());
}
