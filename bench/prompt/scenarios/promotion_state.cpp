// bench/prompt/scenarios/promotion_state.cpp
//
// Bounded promotion-state churn plus deterministic snapshot extraction.

#include <nanobench.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include <oran/core/time.hpp>
#include <oran/prompt.hpp>

namespace orangutan::bench {
namespace core = orangutan::core;
namespace prompt = orangutan::prompt;

namespace {

core::Time at_seconds(int seconds) {
  return core::Time{core::Time::time_point{std::chrono::seconds{seconds}}};
}

std::vector<std::string> promotion_names() {
  std::vector<std::string> names;
  names.reserve(20);
  for (int i = 0; i < 20; ++i) {
    names.push_back("deferred.tool." + std::to_string(i));
  }
  return names;
}

}  // namespace

void register_promotion_state(ankerl::nanobench::Bench& bench) {
  constexpr auto kBatch = 128;
  const auto names = promotion_names();

  bench.batch(kBatch);
  bench.run("prompt.promotion_state_promote_snapshot", [&] {
    auto total_evictions = std::uint64_t{0};
    for (auto batch_index = 0; batch_index < kBatch; ++batch_index) {
      prompt::PromotionState state;
      for (std::size_t i = 0; i < names.size(); ++i) {
        auto result = state.promote(names[i], at_seconds(static_cast<int>(i)));
        if (!result) {
          std::abort();
        }
      }
      auto snapshot = state.snapshot(at_seconds(25));
      total_evictions += snapshot.stats.evictions_lru;
      ankerl::nanobench::doNotOptimizeAway(snapshot.tool_names.size());
    }
    ankerl::nanobench::doNotOptimizeAway(total_evictions);
  });
}

}  // namespace orangutan::bench
