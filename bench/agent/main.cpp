// bench/agent/main.cpp — nanobench entry point for oran-agent.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <print>

namespace orangutan::bench {
void register_prompt_cache_hit_rate(ankerl::nanobench::Bench& bench);
}  // namespace orangutan::bench

int main() {
  ankerl::nanobench::Bench bench;
  bench.title("bench-agent").relative(true);
  bench.unit("fixture");
  bench.minEpochIterations(12'000);
  bench.warmup(20);

  orangutan::bench::register_prompt_cache_hit_rate(bench);

  std::println();
  return 0;
}
