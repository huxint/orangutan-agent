// bench/cli/scenarios/dispatch.cpp
//
// A-vs-B comparison: single-shot prompt dispatch vs. empty REPL dispatch shell.

#include <nanobench.h>

#include <cstdlib>
#include <span>
#include <string_view>
#include <vector>

#include <oran/cli.hpp>

namespace orangutan::bench {
namespace cli = orangutan::cli;

namespace {

[[gnu::noinline]] std::size_t run_cli(const cli::CliOptions& options) {
  auto result = cli::run(options);
  if (!result) {
    std::abort();
  }
  return result->prompts_processed + static_cast<std::size_t>(result->mode);
}

}  // namespace

void register_cli_dispatch(ankerl::nanobench::Bench& bench) {
  auto prompt_args = std::vector<std::string_view>{"--prompt", "What is 17 * 23?"};
  const auto prompt_options = cli::CliOptions{.args = std::span<const std::string_view>{prompt_args}, .quiet = true};
  const auto repl_options = cli::CliOptions{.quiet = true};

  bench.run("cli.single_shot_prompt", [&] {
    const auto value = run_cli(prompt_options);
    ankerl::nanobench::doNotOptimizeAway(value);
  });
  bench.run("cli.repl_empty", [&] {
    const auto value = run_cli(repl_options);
    ankerl::nanobench::doNotOptimizeAway(value);
  });
}

}  // namespace orangutan::bench
