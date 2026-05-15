// bench/io/main.cpp — registers and runs the oran-io nanobench scenarios.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <print>

namespace orangutan::bench {
void register_file_read(ankerl::nanobench::Bench&);
}

int main() {
  ankerl::nanobench::Bench b;
  b.title("bench-io");
  b.unit("read");
  b.minEpochIterations(12'000);
  b.warmup(100);

  orangutan::bench::register_file_read(b);

  std::println();
  return 0;
}
