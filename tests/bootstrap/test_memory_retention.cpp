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

TEST_CASE("cron_jobs_from maps automation cron config into repository seeds", "[unit][bootstrap][automation]") {
  auto cfg = config::Config::parse(R"json({
  "automation": {
    "cron": {
      "jobs": [
        {
          "job_key": "daily-summary",
          "agent_key": "researcher",
          "agent_prompt": "Summarize yesterday's repository activity.",
          "expression": "0 9 * * *",
          "first_fire_at": "2026-06-08T09:00:00Z"
        },
        {
          "job_key": "hourly-ci",
          "agent_prompt": "Check CI status and summarize failures.",
          "expression": "15 * * * *",
          "first_fire_at": "2026-06-08T00:15:00Z",
          "last_fired_at": "2026-06-08T03:15:00Z"
        }
      ]
    }
  }
})json");
  REQUIRE(cfg.has_value());

  auto jobs = bootstrap::cron_jobs_from(*cfg);

  REQUIRE(jobs.has_value());
  REQUIRE(jobs->size() == 2);
  REQUIRE((*jobs)[0].job_key == "daily-summary");
  REQUIRE((*jobs)[0].agent_key == "researcher");
  REQUIRE((*jobs)[0].agent_prompt == "Summarize yesterday's repository activity.");
  REQUIRE((*jobs)[0].schedule.expression == "0 9 * * *");
  REQUIRE(core::time::format_iso8601_utc((*jobs)[0].schedule.first_fire_at) == "2026-06-08T09:00:00.000Z");
  REQUIRE_FALSE((*jobs)[0].state.last_fired_at.has_value());

  REQUIRE((*jobs)[1].job_key == "hourly-ci");
  REQUIRE((*jobs)[1].agent_key == "automation");
  REQUIRE((*jobs)[1].agent_prompt == "Check CI status and summarize failures.");
  REQUIRE((*jobs)[1].schedule.expression == "15 * * * *");
  REQUIRE((*jobs)[1].state.last_fired_at.has_value());
}

TEST_CASE("cron_jobs_from rejects invalid cron expressions at bootstrap composition", "[unit][bootstrap][automation]") {
  auto cfg = config::Config::parse(R"json({
  "automation": {
    "cron": {
      "jobs": [{
        "job_key": "bad-cron",
        "agent_prompt": "Run invalid cron for validation.",
        "expression": "not a cron",
        "first_fire_at": "2026-06-08T00:00:00Z"
      }]
    }
  }
})json");
  REQUIRE(cfg.has_value());

  auto jobs = bootstrap::cron_jobs_from(*cfg);

  REQUIRE_FALSE(jobs.has_value());
  REQUIRE(jobs.error().kind() == core::ErrorKind::config);
}

TEST_CASE("triggered_jobs_from maps automation triggered config into repository seeds",
          "[unit][bootstrap][automation]") {
  auto cfg = config::Config::parse(R"json({
  "automation": {
    "triggered": {
      "jobs": [
        {
          "job_key": "triggered-ci",
          "trigger_key": "webhook:ci",
          "agent_key": "researcher",
          "agent_prompt": "Investigate the incoming CI webhook."
        },
        {
          "job_key": "triggered-alert",
          "trigger_key": "channel:mock-main:ops",
          "agent_prompt": "Handle the operator alert."
        }
      ]
    }
  }
})json");
  REQUIRE(cfg.has_value());

  auto jobs = bootstrap::triggered_jobs_from(*cfg);

  REQUIRE(jobs.has_value());
  REQUIRE(jobs->size() == 2);
  REQUIRE((*jobs)[0].job_key == "triggered-ci");
  REQUIRE((*jobs)[0].trigger_key == "webhook:ci");
  REQUIRE((*jobs)[0].agent_key == "researcher");
  REQUIRE((*jobs)[0].agent_prompt == "Investigate the incoming CI webhook.");

  REQUIRE((*jobs)[1].job_key == "triggered-alert");
  REQUIRE((*jobs)[1].trigger_key == "channel:mock-main:ops");
  REQUIRE((*jobs)[1].agent_key == "automation");
  REQUIRE((*jobs)[1].agent_prompt == "Handle the operator alert.");
}
