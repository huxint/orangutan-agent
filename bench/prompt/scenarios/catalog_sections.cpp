// bench/prompt/scenarios/catalog_sections.cpp
//
// A-vs-B comparison: default active set vs explicit active subset.

#include <nanobench.h>

#include <cstdlib>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <oran/config.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/prompt.hpp>

namespace orangutan::bench {
namespace config = orangutan::config;
namespace core = orangutan::core;
namespace prompt = orangutan::prompt;

namespace {

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
      tool_def("FileRead", "Read a file"),
      tool_def("FileWrite", "Write a file"),
      tool_def("FileEdit", "Edit a file"),
      tool_def("FileSearch", "Search files"),
      tool_def("DirectoryList", "List a directory"),
      tool_def("ToolSearch", "Search tools"),
      tool_def("MemoryRecall", "Recall memory", true),
      tool_def("AgentSpawn", "Spawn an agent", true),
  };
}

prompt::RenderedPrompt build_once(prompt::Builder& builder,
                                  const std::vector<core::ToolDef>& catalog,
                                  config::PromptActiveToolsConfig active_tools = {},
                                  std::span<const std::string> promoted_tools = {}) {
  asio::io_context io;
  prompt::RenderedPrompt rendered;
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        auto result = co_await builder.build(prompt::BuilderInputs{
            .system_preamble = "system",
            .tool_catalog = catalog,
            .active_tools = std::move(active_tools),
            .promoted_tools = promoted_tools,
            .skills_catalog = "",
            .memory_framing = "",
            .per_agent_overlay = "",
            .conversation_tail = {},
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

}  // namespace

void register_catalog_sections(ankerl::nanobench::Bench& bench) {
  auto catalog = make_catalog();
  prompt::Builder default_builder;
  prompt::Builder explicit_builder;
  auto explicit_tools = config::PromptActiveToolsConfig{
      .use_defaults = false,
      .tool_names = {"FileRead", "ToolSearch"},
  };
  const std::vector<std::string> promoted_tools{"MemoryRecall", "AgentSpawn"};

  bench.run("prompt.build_default_active_set", [&] {
    auto rendered = build_once(default_builder, catalog);
    ankerl::nanobench::doNotOptimizeAway(rendered.prefix_hash);
    ankerl::nanobench::doNotOptimizeAway(rendered.prefix_bytes);
  });

  bench.run("prompt.build_explicit_subset", [&] {
    auto rendered = build_once(explicit_builder, catalog, explicit_tools);
    ankerl::nanobench::doNotOptimizeAway(rendered.prefix_hash);
    ankerl::nanobench::doNotOptimizeAway(rendered.prefix_bytes);
  });

  bench.run("prompt.build_promoted_subset", [&] {
    auto rendered = build_once(default_builder, catalog, {}, promoted_tools);
    ankerl::nanobench::doNotOptimizeAway(rendered.prefix_hash);
    ankerl::nanobench::doNotOptimizeAway(rendered.prefix_bytes);
  });
}

}  // namespace orangutan::bench
