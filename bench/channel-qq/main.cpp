// bench/channel-qq/main.cpp — registers and runs oran-channel-qq nanobench scenarios.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <print>

namespace orangutan::bench {
void register_channel_qq_normalize_response(ankerl::nanobench::Bench&);
}  // namespace orangutan::bench

int main() {
  ankerl::nanobench::Bench b;
  b.title("bench-channel-qq");
  b.unit("response");
  b.minEpochIterations(20'000);
  b.warmup(100);

  orangutan::bench::register_channel_qq_normalize_response(b);

  std::println();
  return 0;
}
