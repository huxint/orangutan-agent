// bench/skill/main.cpp - nanobench entry point for oran-skill.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <print>

namespace orangutan::bench {
void register_skill_catalog(ankerl::nanobench::Bench& bench);
}  // namespace orangutan::bench

int main() {
  ankerl::nanobench::Bench bench;
  bench.title("bench-skill").relative(true);
  bench.unit("catalog");
  bench.minEpochIterations(1000);
  bench.warmup(50);

  orangutan::bench::register_skill_catalog(bench);

  std::println();
  return 0;
}
