// bench/tool/scenarios/output.cpp
//
// A-vs-B coverage for the structured output envelope:
//
//   1. `output.text_only`      : v1-compatible text-only construction.
//   2. `output.with_data_16kib`: structured payload metadata construction.

#include <nanobench.h>

#include <string>

#include <oran/tool/output.hpp>

namespace orangutan::bench {

void register_tool_output(ankerl::nanobench::Bench& bench) {
  bench.run("output.text_only", [] {
    auto output = tool::Output::text_only("ok");
    ankerl::nanobench::doNotOptimizeAway(output);
  });

  const auto data = std::string(16U * 1024U, 'x');
  bench.run("output.with_data_16kib", [&] {
    auto output = tool::Output{
        .text = "structured",
        .data_json = data,
        .usage =
            tool::ToolUsage{
                .bytes_read = data.size(),
                .files_touched = 1,
            },
    };
    ankerl::nanobench::doNotOptimizeAway(output);
  });
}

}  // namespace orangutan::bench
