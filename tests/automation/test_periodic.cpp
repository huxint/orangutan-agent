// tests/automation/test_periodic.cpp — periodic automation planning coverage.

#include <algorithm>
#include <chrono>
#include <limits>
#include <ranges>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <oran/automation.hpp>
#include <oran/core/error.hpp>

namespace automation = orangutan::automation;
namespace core = orangutan::core;
namespace memory = orangutan::memory;

namespace {

using namespace std::chrono_literals;

[[nodiscard]] core::Time at(std::chrono::seconds value) {
  return core::Time{core::Time::time_point{value}};
}

[[nodiscard]] bool has_field(const core::Error& error, std::string_view field) {
  return std::ranges::any_of(error.context(),
                             [field](const auto& entry) { return entry.first == "field" && entry.second == field; });
}

}  // namespace

TEST_CASE("evaluate_periodic_schedule fires a never-run job at its anchor", "[unit][automation][periodic]") {
  auto result =
      automation::evaluate_periodic_schedule(automation::PeriodicSchedule{.first_fire_at = at(10s), .interval = 15s},
                                             {},
                                             at(10s));

  REQUIRE(result.has_value());
  REQUIRE(result->due);
  REQUIRE(result->next_fire_at == at(10s));
  REQUIRE(result->overdue_by == 0ns);
}

TEST_CASE("evaluate_periodic_schedule reports the next fire before it is due", "[unit][automation][periodic]") {
  auto result =
      automation::evaluate_periodic_schedule(automation::PeriodicSchedule{.first_fire_at = at(10s), .interval = 15s},
                                             {},
                                             at(9s));

  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->due);
  REQUIRE(result->next_fire_at == at(10s));
  REQUIRE(result->overdue_by == 0ns);
}

TEST_CASE("evaluate_periodic_schedule advances from the last fired time", "[unit][automation][periodic]") {
  auto result =
      automation::evaluate_periodic_schedule(automation::PeriodicSchedule{.first_fire_at = at(10s), .interval = 15s},
                                             automation::PeriodicJobState{.last_fired_at = at(25s)},
                                             at(43s));

  REQUIRE(result.has_value());
  REQUIRE(result->due);
  REQUIRE(result->next_fire_at == at(40s));
  REQUIRE(result->overdue_by == 3s);
}

TEST_CASE("evaluate_periodic_schedule rejects non-positive intervals", "[unit][automation][periodic]") {
  auto result =
      automation::evaluate_periodic_schedule(automation::PeriodicSchedule{.first_fire_at = at(10s), .interval = 0s},
                                             {},
                                             at(10s));

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  REQUIRE(has_field(result.error(), "interval"));
}

TEST_CASE("plan_memory_retention returns no decay request before cadence is due",
          "[unit][automation][periodic][memory]") {
  auto result = automation::plan_memory_retention(
      automation::MemoryRetentionJob{
          .scope_key = "cli",
          .policy =
              automation::LongtermMemoryRetentionPolicy{
                  .forget_after_unused = std::chrono::days{30},
                  .importance_floor = 0.25,
                  .max_records_per_scope = 250,
                  .decay_check_interval = 24h,
              },
          .first_fire_at = at(100s),
      },
      {},
      at(99s));

  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->schedule.due);
  REQUIRE_FALSE(result->decay_request.has_value());
}

TEST_CASE("plan_memory_retention builds a scoped decay request when cadence is due",
          "[unit][automation][periodic][memory]") {
  const auto now = core::Time{core::Time::time_point{std::chrono::days{45}}};
  auto result = automation::plan_memory_retention(
      automation::MemoryRetentionJob{
          .scope_key = "cli",
          .policy =
              automation::LongtermMemoryRetentionPolicy{
                  .forget_after_unused = std::chrono::days{30},
                  .importance_floor = 0.25,
                  .max_records_per_scope = 250,
                  .decay_check_interval = 24h,
              },
          .first_fire_at = core::Time::epoch(),
      },
      automation::PeriodicJobState{.last_fired_at = core::Time{core::Time::time_point{std::chrono::days{43}}}},
      now);

  REQUIRE(result.has_value());
  REQUIRE(result->schedule.due);
  REQUIRE(result->schedule.next_fire_at == core::Time{core::Time::time_point{std::chrono::days{44}}});
  REQUIRE(result->decay_request.has_value());
  REQUIRE(result->decay_request->scope_key == "cli");
  REQUIRE(result->decay_request->unused_before == core::Time{core::Time::time_point{std::chrono::days{15}}});
  REQUIRE(result->decay_request->importance_floor == 0.25);
  REQUIRE(result->decay_request->limit == 250);
  REQUIRE(result->decay_request->decay_at == now);
}

TEST_CASE("plan_memory_retention validates retention job inputs", "[unit][automation][periodic][memory]") {
  auto job = automation::MemoryRetentionJob{
      .scope_key = "",
      .policy = automation::LongtermMemoryRetentionPolicy{.decay_check_interval = 24h},
      .first_fire_at = core::Time::epoch(),
  };

  auto result = automation::plan_memory_retention(job, {}, core::Time::epoch());
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  REQUIRE(has_field(result.error(), "scope_key"));

  job.scope_key = "cli";
  job.policy.importance_floor = 1.1;
  result = automation::plan_memory_retention(job, {}, core::Time::epoch());
  REQUIRE_FALSE(result.has_value());
  REQUIRE(has_field(result.error(), "importance_floor"));

  job.policy.importance_floor = 0.0;
  job.policy.importance_floor = std::numeric_limits<double>::quiet_NaN();
  result = automation::plan_memory_retention(job, {}, core::Time::epoch());
  REQUIRE_FALSE(result.has_value());
  REQUIRE(has_field(result.error(), "importance_floor"));

  job.policy.importance_floor = 0.0;
  job.policy.max_records_per_scope = 0;
  result = automation::plan_memory_retention(job, {}, core::Time::epoch());
  REQUIRE_FALSE(result.has_value());
  REQUIRE(has_field(result.error(), "max_records_per_scope"));

  job.policy.max_records_per_scope = 10000;
  job.policy.forget_after_unused = std::chrono::days{0};
  result = automation::plan_memory_retention(job, {}, core::Time::epoch());
  REQUIRE_FALSE(result.has_value());
  REQUIRE(has_field(result.error(), "forget_after_unused"));

  job.policy.forget_after_unused = std::chrono::days{180};
  job.policy.decay_check_interval = 0h;
  result = automation::plan_memory_retention(job, {}, core::Time::epoch());
  REQUIRE_FALSE(result.has_value());
  REQUIRE(has_field(result.error(), "decay_check_interval"));
}
