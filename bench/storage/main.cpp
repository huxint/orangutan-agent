// bench/storage/main.cpp — registers and runs the oran-storage nanobench scenarios.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <print>

namespace orangutan::bench {
void register_migrations(ankerl::nanobench::Bench&);
void register_sqlite_insert(ankerl::nanobench::Bench&);
}  // namespace orangutan::bench

int main() {
  ankerl::nanobench::Bench b;
  b.title("bench-storage");
  b.unit("batch");
  b.minEpochIterations(1000);
  b.warmup(100);

  orangutan::bench::register_migrations(b);
  orangutan::bench::register_sqlite_insert(b);

  std::println();
  return 0;
}
