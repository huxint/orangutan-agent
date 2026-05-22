// bench/core/scenarios/bounded_cache.cpp
//
// A/B comparison: raw `std::unordered_map<int,int>` insert+lookup vs.
// `core::BoundedCache<int,int>` insert+lookup with LRU bookkeeping.
// The delta is what the cache pays for eviction ordering, TTL checks,
// and stats accounting — useful when sizing per-call cache budgets in
// later slices (regex cache, line-offset index, tool block cache).

#include <nanobench.h>

#include <chrono>
#include <cstddef>
#include <unordered_map>

#include <oran/core/bounded_cache.hpp>
#include <oran/core/time.hpp>

namespace orangutan::bench {

namespace {

constexpr std::size_t kWorkingSetEntries = 256;
constexpr std::size_t kProbeCount = 1024;

orangutan::core::Time at_iteration(std::size_t iter) noexcept {
  return orangutan::core::Time{orangutan::core::Time::clock::time_point{std::chrono::seconds{static_cast<int>(iter)}}};
}

}  // namespace

void register_bounded_cache_scenarios(ankerl::nanobench::Bench& bench) {
  bench.run("core.unordered_map_insert_lookup_256", [&] {
    std::unordered_map<int, int> map;
    map.reserve(kWorkingSetEntries);
    int sum = 0;
    for (std::size_t i = 0; i < kProbeCount; ++i) {
      const int key = static_cast<int>(i % kWorkingSetEntries);
      auto [it, inserted] = map.try_emplace(key, key);
      sum += it->second;
    }
    ankerl::nanobench::doNotOptimizeAway(sum);
  });

  bench.run("core.bounded_cache_insert_lookup_256", [&] {
    orangutan::core::BoundedCache<int, int> cache{
        orangutan::core::BoundedCache<int, int>::Options{.max_entries = kWorkingSetEntries},
    };
    int sum = 0;
    for (std::size_t i = 0; i < kProbeCount; ++i) {
      const int key = static_cast<int>(i % kWorkingSetEntries);
      if (auto* hit = cache.get(key, at_iteration(i))) {
        sum += *hit;
        continue;
      }
      cache.put(key, key, at_iteration(i));
      sum += key;
    }
    ankerl::nanobench::doNotOptimizeAway(sum);
  });

  bench.run("core.bounded_cache_overflow_lru_eviction", [&] {
    // The probe set is larger than the cache capacity, so every put
    // triggers an LRU eviction — this scenario pins the eviction path.
    orangutan::core::BoundedCache<int, int> cache{
        orangutan::core::BoundedCache<int, int>::Options{.max_entries = kWorkingSetEntries / 4},
    };
    int sum = 0;
    for (std::size_t i = 0; i < kProbeCount; ++i) {
      const int key = static_cast<int>(i);
      cache.put(key, key, at_iteration(i));
      sum += key;
    }
    ankerl::nanobench::doNotOptimizeAway(sum);
  });
}

}  // namespace orangutan::bench
