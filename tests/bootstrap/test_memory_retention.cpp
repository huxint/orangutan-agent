// tests/bootstrap/test_memory_retention.cpp - bootstrap retention mapping coverage.

#include <chrono>

#include <catch2/catch_test_macros.hpp>

#include <oran/bootstrap.hpp>
#include <oran/config.hpp>
#include <oran/core/time.hpp>

namespace automation = orangutan::automation;
namespace bootstrap = orangutan::bootstrap;
namespace config = orangutan::config;
namespace core = orangutan::core;

namespace {

using namespace std::chrono_literals;

[[nodiscard]] core::Time at(std::chrono::seconds value) {
  return core::Time{core::Time::time_point{value}};
}

}  // namespace

TEST_CASE("longterm_memory_retention_job_from maps config policy into automation units", "[unit][bootstrap][memory]") {
  auto cfg = config::Config::parse(R"json(
{
  "memory": {
    "longterm": {
      "retention": {
        "forget_after_unused_days": 3,
        "importance_floor": 0.5,
        "max_records_per_scope": 7,
        "decay_check_interval_hours": 6
      }
    }
  }
}
)json");
  REQUIRE(cfg.has_value());

  const auto first_fire_at = at(60s);
  auto job = bootstrap::longterm_memory_retention_job_from(*cfg, "cli", first_fire_at);

  REQUIRE(job.has_value());
  REQUIRE(job->scope_key == "cli");
  REQUIRE(job->first_fire_at == first_fire_at);
  REQUIRE(job->policy.forget_after_unused == std::chrono::days{3});
  REQUIRE(job->policy.importance_floor == 0.5);
  REQUIRE(job->policy.max_records_per_scope == 7);
  REQUIRE(job->policy.decay_check_interval == 6h);
}

TEST_CASE("longterm_memory_startup_decay_options_from derives startup decay from the job",
          "[unit][bootstrap][memory]") {
  const auto decay_at = core::Time{core::Time::time_point{std::chrono::days{10}}};
  const auto job = automation::MemoryRetentionJob{
      .scope_key = "cli",
      .policy =
          automation::LongtermMemoryRetentionPolicy{
              .forget_after_unused = std::chrono::days{3},
              .importance_floor = 0.5,
              .max_records_per_scope = 7,
              .decay_check_interval = 6h,
          },
      .first_fire_at = core::Time{decay_at.to_system_time_point() + 6h},
  };

  const auto options = bootstrap::longterm_memory_startup_decay_options_from(job, decay_at);

  REQUIRE(options.scope_key == "cli");
  REQUIRE(options.unused_before == core::Time{core::Time::time_point{std::chrono::days{7}}});
  REQUIRE(options.importance_floor == 0.5);
  REQUIRE(options.limit == 7);
  REQUIRE(options.decay_at == decay_at);
}
