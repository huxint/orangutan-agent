#include <nanobench.h>

#include <span>
#include <vector>

#include <oran/core/tool_def.hpp>
#include <oran/prompt.hpp>

namespace orangutan::bench {

void register_catalog_sections(ankerl::nanobench::Bench& bench) {
  const std::vector<core::ToolDef> tools{
      core::ToolDef::with_no_input("AgentRun", "Run a configured child agent"),
      core::ToolDef::with_no_input("FileEdit", "Edit a file"),
      core::ToolDef::with_no_input("FileRead", "Read a file"),
      core::ToolDef::with_no_input("FileWrite", "Write a file"),
      core::ToolDef::with_no_input("MemoryForget", "Forget a note"),
      core::ToolDef::with_no_input("MemoryRecall", "Read a note"),
      core::ToolDef::with_no_input("MemoryRemember", "Save a note"),
  };

  bench.run("prompt.render_native_catalog", [&tools] {
    const auto rendered = prompt::render({.system_preamble = "system", .tools = tools});
    ankerl::nanobench::doNotOptimizeAway(rendered.prefix_hash);
    ankerl::nanobench::doNotOptimizeAway(rendered.prefix_bytes);
  });

  bench.run("prompt.render_native_subset", [&tools] {
    const auto rendered = prompt::render({.system_preamble = "system", .tools = std::span{tools}.first(2)});
    ankerl::nanobench::doNotOptimizeAway(rendered.prefix_hash);
    ankerl::nanobench::doNotOptimizeAway(rendered.prefix_bytes);
  });
}

}  // namespace orangutan::bench
