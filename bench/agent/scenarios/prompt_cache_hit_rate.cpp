// bench/agent/scenarios/prompt_cache_hit_rate.cpp
//
// A-vs-B comparison: stable prompt-cache fixture without promotions vs. after
// the session observes a `tool.search` result that promotes one deferred tool.

#include <nanobench.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <oran/agent.hpp>
#include <oran/config.hpp>
#include <oran/core/message.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/prompt.hpp>
#include <oran/tool.hpp>

namespace orangutan::bench {
namespace agent = orangutan::agent;
namespace config = orangutan::config;
namespace core = orangutan::core;
namespace prompt = orangutan::prompt;
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
      .category = "bench",
  };
}

std::vector<core::ToolDef> make_catalog() {
  return {
      tool_def("file.read", "Read a file"),
      tool_def("file.write", "Write a file"),
      tool_def("file.edit", "Edit a file"),
      tool_def("file.search", "Search files"),
      tool_def("directory.list", "List a directory"),
      tool_def("tool.search", "Search tools"),
      tool_def("memory.recall", "Recall memory", true),
      tool_def("agent.spawn", "Spawn an agent", true),
  };
}

prompt::RenderedPrompt build_once(prompt::Builder& builder,
                                  const std::vector<core::ToolDef>& catalog,
                                  const std::vector<std::string>& promoted_tools,
                                  std::string tail_text) {
  asio::io_context io;
  prompt::RenderedPrompt rendered;
  std::vector<core::Message> tail{core::Message::user_text(std::move(tail_text))};
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        auto result = co_await builder.build(prompt::BuilderInputs{
            .system_preamble = "system",
            .tool_catalog = catalog,
            .active_tools = config::PromptActiveToolsConfig{},
            .promoted_tools = promoted_tools,
            .skills_catalog = "",
            .memory_framing = "",
            .per_agent_overlay = "",
            .conversation_tail = tail,
        });
        if (!result) {
          std::abort();
        }
        rendered = std::move(*result);
      },
      asio::detached);
  io.run();
  return rendered;
}

std::uint64_t run_prompt_cache_fixture(bool with_promotion) {
  auto catalog = make_catalog();
  agent::SessionState session;
  if (with_promotion) {
    auto observed = session.observe_tool_output(
        tool::kToolSearchName,
        tool::Output{
            .text = "tool.search: 1 match",
            .data_json = R"({"kind":"tool_search","matches":[{"name":"memory.recall","deferred":true}]})",
        },
        at_seconds(1));
    if (!observed || observed->promoted != 1) {
      std::abort();
    }
  }

  prompt::Builder builder;
  std::optional<std::uint64_t> stable_hash;
  std::uint64_t last_hash = 0;
  for (int i = 0; i < 8; ++i) {
    auto snapshot = session.promotion_snapshot(at_seconds(2));
    auto rendered = build_once(builder, catalog, snapshot.tool_names, "turn " + std::to_string(i));
    if (!stable_hash.has_value()) {
      stable_hash = rendered.prefix_hash;
    } else if (rendered.prefix_hash != *stable_hash) {
      std::abort();
    }
    last_hash = rendered.prefix_hash;
  }
  return last_hash;
}

}  // namespace

void register_prompt_cache_hit_rate(ankerl::nanobench::Bench& bench) {
  bench.run("agent.prompt_cache_no_promotions", [] {
    const auto hash = run_prompt_cache_fixture(false);
    ankerl::nanobench::doNotOptimizeAway(hash);
  });

  bench.run("agent.prompt_cache_after_promotion", [] {
    const auto hash = run_prompt_cache_fixture(true);
    ankerl::nanobench::doNotOptimizeAway(hash);
  });
}

}  // namespace orangutan::bench
