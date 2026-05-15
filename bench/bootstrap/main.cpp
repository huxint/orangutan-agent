// bench/bootstrap/main.cpp — registers and runs the oran-bootstrap nanobench scenarios.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <print>

namespace orangutan::bench {
void register_config_startup(ankerl::nanobench::Bench&);
}  // namespace orangutan::bench

int main() {
  ankerl::nanobench::Bench b;
  b.title("bench-bootstrap");
  b.unit("load");
  b.minEpochIterations(150000);
  b.warmup(100);

  orangutan::bench::register_config_startup(b);

  std::println();
  return 0;
}
