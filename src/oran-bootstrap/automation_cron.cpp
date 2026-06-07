// src/oran-bootstrap/automation_cron.cpp - config-to-automation cron mapping.

#include <oran/bootstrap/automation_cron.hpp>

#include <expected>
#include <string>
#include <utility>
#include <vector>

#include <oran/config.hpp>
#include <oran/core/error.hpp>

namespace orangutan::bootstrap {
namespace {

using ::orangutan::core::Error;

[[nodiscard]] Error cron_config_error(std::string message, std::string job_key) {
  auto error = Error::config(std::move(message)).with("path", "$.automation.cron.jobs");
  if (!job_key.empty()) {
    error.with("job_key", std::move(job_key));
  }
  return error;
}

}  // namespace

core::Result<std::vector<automation::UpsertCronJobRequest>> cron_jobs_from(const config::Config& cfg) {
  auto seeds = std::vector<automation::UpsertCronJobRequest>{};
  seeds.reserve(cfg.automation().cron.jobs.size());
  for (const auto& job : cfg.automation().cron.jobs) {
    auto request = automation::UpsertCronJobRequest{
        .job_key = job.job_key,
        .agent_key = job.agent_key,
        .schedule =
            automation::CronSchedule{
                .expression = job.expression,
                .first_fire_at = job.first_fire_at,
            },
        .state =
            automation::PeriodicJobState{
                .last_fired_at = job.last_fired_at,
            },
    };
    auto evaluated =
        automation::evaluate_cron_schedule(request.schedule, request.state, request.schedule.first_fire_at);
    if (!evaluated) {
      return std::unexpected(cron_config_error("invalid automation cron expression", request.job_key)
                                 .with("reason", std::string{evaluated.error().message()}));
    }
    seeds.push_back(std::move(request));
  }
  return seeds;
}

}  // namespace orangutan::bootstrap
