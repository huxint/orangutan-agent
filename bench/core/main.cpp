// bench/core/main.cpp — registers and runs the oran-core nanobench scenarios.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <print>

namespace orangutan::bench {
void register_error_construct(ankerl::nanobench::Bench&);
}

int main() {
  ankerl::nanobench::Bench b;
  b.title("bench-core");
  b.unit("Error");
  b.minEpochIterations(200'000);
  b.warmup(2'000);

  orangutan::bench::register_error_construct(b);

  std::println();
  return 0;
}
