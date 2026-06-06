// bench/automation/main.cpp — nanobench entry point for oran-automation.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <print>

namespace orangutan::bench {
void register_automation_periodic(ankerl::nanobench::Bench& bench);
}  // namespace orangutan::bench

int main() {
  ankerl::nanobench::Bench bench;
  bench.title("bench-automation").relative(true);
  bench.unit("batch");
  bench.minEpochIterations(10'000);
  bench.warmup(50);

  orangutan::bench::register_automation_periodic(bench);

  std::println();
  return 0;
}
