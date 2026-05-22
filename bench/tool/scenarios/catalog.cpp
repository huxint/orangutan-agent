// bench/tool/scenarios/catalog.cpp
//
// A-vs-B coverage for deterministic tool-catalog rendering:
//
//   1. `catalog.render_cold_32_tools` : first render of a 32-tool catalog.
//   2. `catalog.render_hot_32_tools`  : repeat render through the bounded
//                                       rendered-block cache.

#include <nanobench.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include <oran/core/capability.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/tool/catalog.hpp>

namespace orangutan::bench {

namespace {

[[nodiscard]] core::ToolDef make_tool(std::uint32_t index) {
  return core::ToolDef{
      .name = "tool." + std::to_string(index),
      .description = "Synthetic catalog tool " + std::to_string(index) + ".",
      .input_schema_json =
          R"({"type":"object","properties":{"path":{"type":"string"},"max_bytes":{"type":"integer","minimum":1}},"required":["path"],"additionalProperties":false})",
      .required_capabilities = {core::Capability::read_file},
      .deferred = (index % 5) == 0,
      .category = "bench",
  };
}

[[nodiscard]] std::vector<core::ToolDef> make_catalog() {
  std::vector<core::ToolDef> defs;
  defs.reserve(32);
  for (std::uint32_t i = 0; i < 32; ++i) {
    defs.push_back(make_tool(i));
  }
  return defs;
}

}  // namespace

void register_tool_catalog(ankerl::nanobench::Bench& bench) {
  const auto defs = make_catalog();

  bench.run("catalog.render_cold_32_tools", [&] {
    tool::CatalogRenderer renderer;
    auto rendered = renderer.render_catalog(defs);
    if (!rendered.has_value()) {
      std::abort();
    }
    ankerl::nanobench::doNotOptimizeAway(rendered->active_text);
    ankerl::nanobench::doNotOptimizeAway(rendered->deferred_text);
  });

  tool::CatalogRenderer hot_renderer;
  auto warmed = hot_renderer.render_catalog(defs);
  if (!warmed.has_value()) {
    std::abort();
  }
  bench.run("catalog.render_hot_32_tools", [&] {
    auto rendered = hot_renderer.render_catalog(defs);
    if (!rendered.has_value()) {
      std::abort();
    }
    ankerl::nanobench::doNotOptimizeAway(rendered->active_text);
    ankerl::nanobench::doNotOptimizeAway(rendered->deferred_text);
  });
}

}  // namespace orangutan::bench
