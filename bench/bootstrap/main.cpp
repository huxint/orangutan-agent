// bench/bootstrap/main.cpp — registers and runs the oran-bootstrap nanobench scenarios.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <print>

namespace orangutan::bench {
void register_config_startup(ankerl::nanobench::Bench&);
void register_runtime_assembly_build(ankerl::nanobench::Bench&);
}  // namespace orangutan::bench

int main() {
  ankerl::nanobench::Bench b;
  b.title("bench-bootstrap");
  b.unit("load");
  b.minEpochIterations(150000);
  b.warmup(100);

  orangutan::bench::register_config_startup(b);
  // The assembly scenarios are ms-scale; their registration lowers the
  // iteration count so the run stays bounded. Keeping the call last
  // means earlier µs-scale scenarios are unaffected.
  orangutan::bench::register_runtime_assembly_build(b);

  std::println();
  return 0;
}
