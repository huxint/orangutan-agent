#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <print>

namespace orangutan::bench {
void register_runtime_assembly_build(ankerl::nanobench::Bench&);
}  // namespace orangutan::bench

int main() {
  ankerl::nanobench::Bench b;
  b.title("bench-bootstrap");
  b.unit("assembly");
  orangutan::bench::register_runtime_assembly_build(b);

  std::println();
  return 0;
}
