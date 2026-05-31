// bench/memory/main.cpp — nanobench entry point for oran-memory.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <print>

namespace orangutan::bench {
void register_session_store(ankerl::nanobench::Bench& bench);
}  // namespace orangutan::bench

int main() {
  ankerl::nanobench::Bench bench;
  bench.title("bench-memory").relative(true);
  bench.unit("batch");
  bench.minEpochIterations(500);
  bench.warmup(50);

  orangutan::bench::register_session_store(bench);

  std::println();
  return 0;
}
