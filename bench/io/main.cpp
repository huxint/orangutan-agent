// bench/io/main.cpp — registers and runs the oran-io nanobench scenarios.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <print>

namespace orangutan::bench {
void register_file_read(ankerl::nanobench::Bench&);
void register_fingerprint(ankerl::nanobench::Bench&);
}  // namespace orangutan::bench

int main() {
  ankerl::nanobench::Bench b;
  b.title("bench-io");
  b.unit("read");
  b.minEpochIterations(12'000);
  b.warmup(100);

  orangutan::bench::register_file_read(b);
  orangutan::bench::register_fingerprint(b);

  std::println();
  return 0;
}
