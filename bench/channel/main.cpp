// bench/channel/main.cpp — registers and runs oran-channel nanobench scenarios.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <print>

namespace orangutan::bench {
void register_channel_manager_fanin(ankerl::nanobench::Bench&);
void register_channel_mock_ingress(ankerl::nanobench::Bench&);
}  // namespace orangutan::bench

int main() {
  ankerl::nanobench::Bench b;
  b.title("bench-channel");
  b.unit("batch");
  b.minEpochIterations(20'000);
  b.warmup(100);

  orangutan::bench::register_channel_manager_fanin(b);
  orangutan::bench::register_channel_mock_ingress(b);

  std::println();
  return 0;
}
