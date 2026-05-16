// bench/permission/main.cpp — registers and runs oran-permission nanobench scenarios.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <print>

namespace orangutan::bench {
void register_rule_set_scenarios(ankerl::nanobench::Bench&);
}  // namespace orangutan::bench

int main() {
  {
    ankerl::nanobench::Bench b;
    b.title("bench-permission/rule_set");
    b.unit("evaluate");
    b.minEpochIterations(200'000);
    b.warmup(2'000);

    orangutan::bench::register_rule_set_scenarios(b);
  }

  std::println();
  return 0;
}
