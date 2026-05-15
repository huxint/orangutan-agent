// bench/cli/main.cpp — registers and runs the oran-cli nanobench scenarios.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <print>

namespace orangutan::bench {
void register_cli_dispatch(ankerl::nanobench::Bench&);
}  // namespace orangutan::bench

int main() {
  ankerl::nanobench::Bench b;
  b.title("bench-cli");
  b.unit("dispatch");
  b.minEpochIterations(150000);
  b.warmup(100);

  orangutan::bench::register_cli_dispatch(b);

  std::println();
  return 0;
}
