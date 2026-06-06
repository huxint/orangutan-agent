// bench/automation/scenarios/periodic.cpp
//
// First automation bucket coverage: deterministic periodic evaluation and the
// memory-retention request planner over one 1024-job batch.

#include <nanobench.h>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <vector>

#include <oran/automation.hpp>

namespace orangutan::bench {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] core::Time at(std::chrono::seconds value) {
  return core::Time{core::Time::time_point{value}};
}

[[nodiscard]] std::vector<automation::PeriodicJobState> make_states() {
  std::vector<automation::PeriodicJobState> states;
  states.reserve(1024);
  for (std::size_t i = 0; i < 1024; ++i) {
    states.push_back(automation::PeriodicJobState{.last_fired_at = at(std::chrono::seconds{static_cast<int>(i)})});
  }
  return states;
}

}  // namespace

void register_automation_periodic(ankerl::nanobench::Bench& bench) {
  const auto states = make_states();
  const auto schedule = automation::PeriodicSchedule{.first_fire_at = core::Time::epoch(), .interval = 15s};
  const auto job = automation::MemoryRetentionJob{
      .scope_key = "cli",
      .policy =
          automation::LongtermMemoryRetentionPolicy{
              .forget_after_unused = std::chrono::days{30},
              .importance_floor = 0.25,
              .max_records_per_scope = 250,
              .decay_check_interval = 24h,
          },
      .first_fire_at = core::Time::epoch(),
  };
  const auto now = at(4096s);

  bench.run("automation.periodic_evaluate_1024", [&] {
    std::size_t due_count = 0;
    for (const auto& state : states) {
      auto evaluated = automation::evaluate_periodic_schedule(schedule, state, now);
      if (!evaluated.has_value()) {
        std::abort();
      }
      if (evaluated->due) {
        ++due_count;
      }
    }
    ankerl::nanobench::doNotOptimizeAway(due_count);
  });

  bench.run("automation.memory_retention_plan_1024", [&] {
    std::size_t request_count = 0;
    for (const auto& state : states) {
      auto planned = automation::plan_memory_retention(job, state, now);
      if (!planned.has_value()) {
        std::abort();
      }
      if (planned->decay_request.has_value()) {
        ++request_count;
      }
    }
    ankerl::nanobench::doNotOptimizeAway(request_count);
  });
}

}  // namespace orangutan::bench
