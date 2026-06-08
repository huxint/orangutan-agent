// src/oran-automation/repository.cpp - automation persistence boundary.

#include <oran/automation/repository.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/storage/migrations.hpp>
#include <oran/storage/pool.hpp>
#include <oran/storage/sqlite.hpp>
#include <oran/storage/statement_cache.hpp>

namespace orangutan::automation {
namespace {

constexpr unsigned char kAutomationRetentionStateBytes[] = {
#embed "migrations/automation/0001-automation-retention-state.sql"
};

constexpr unsigned char kAutomationRetentionLeasesBytes[] = {
#embed "migrations/automation/0002-automation-retention-leases.sql"
};

constexpr unsigned char kAutomationCronJobsBytes[] = {
#embed "migrations/automation/0003-automation-cron-jobs.sql"
};

constexpr unsigned char kAutomationCronRunsBytes[] = {
#embed "migrations/automation/0004-automation-cron-runs.sql"
};

constexpr unsigned char kAutomationCronRunOutcomesBytes[] = {
#embed "migrations/automation/0005-automation-cron-run-outcomes.sql"
};

constexpr unsigned char kAutomationCronLeasesBytes[] = {
#embed "migrations/automation/0006-automation-cron-leases.sql"
};

constexpr unsigned char kAutomationCronAgentLeasesBytes[] = {
#embed "migrations/automation/0007-automation-cron-agent-leases.sql"
};

constexpr unsigned char kAutomationTriggeredJobsBytes[] = {
#embed "migrations/automation/0008-automation-triggered-jobs.sql"
};

constexpr unsigned char kAutomationTriggeredRunsBytes[] = {
#embed "migrations/automation/0009-automation-triggered-runs.sql"
};

constexpr unsigned char kAutomationTriggeredAgentLeasesBytes[] = {
#embed "migrations/automation/0010-automation-triggered-agent-leases.sql"
};

constexpr std::string_view kUpsertCronJobSql = R"sql(
INSERT INTO automation_cron_jobs(
  job_key,
  agent_key,
  agent_prompt,
  expression,
  first_fire_at,
  last_fired_at,
  created_at,
  updated_at
) VALUES (?, ?, ?, ?, ?, ?, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
ON CONFLICT(job_key) DO UPDATE SET
  agent_key = excluded.agent_key,
  agent_prompt = excluded.agent_prompt,
  expression = excluded.expression,
  first_fire_at = excluded.first_fire_at,
  last_fired_at = excluded.last_fired_at,
  updated_at = excluded.updated_at
RETURNING
  job_key,
  agent_key,
  agent_prompt,
  expression,
  first_fire_at,
  last_fired_at,
  created_at,
  updated_at
)sql";

constexpr std::string_view kGetCronJobSql = R"sql(
SELECT
  job_key,
  agent_key,
  agent_prompt,
  expression,
  first_fire_at,
  last_fired_at,
  created_at,
  updated_at
FROM automation_cron_jobs
WHERE job_key = ?
)sql";

constexpr std::string_view kMarkCronJobFiredSql = R"sql(
UPDATE automation_cron_jobs
SET last_fired_at = ?,
    updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
WHERE job_key = ?
RETURNING
  job_key,
  agent_key,
  agent_prompt,
  expression,
  first_fire_at,
  last_fired_at,
  created_at,
  updated_at
)sql";

constexpr std::string_view kListCronJobsSql = R"sql(
SELECT
  job_key,
  agent_key,
  agent_prompt,
  expression,
  first_fire_at,
  last_fired_at,
  created_at,
  updated_at
FROM automation_cron_jobs
ORDER BY updated_at DESC, job_key ASC
LIMIT ?
)sql";

constexpr std::string_view kUpsertTriggeredJobSql = R"sql(
INSERT INTO automation_triggered_jobs(
  job_key,
  trigger_key,
  agent_key,
  agent_prompt,
  created_at,
  updated_at
) VALUES (?, ?, ?, ?, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
ON CONFLICT(job_key) DO UPDATE SET
  trigger_key = excluded.trigger_key,
  agent_key = excluded.agent_key,
  agent_prompt = excluded.agent_prompt,
  updated_at = excluded.updated_at
RETURNING
  job_key,
  trigger_key,
  agent_key,
  agent_prompt,
  created_at,
  updated_at
)sql";

constexpr std::string_view kGetTriggeredJobSql = R"sql(
SELECT
  job_key,
  trigger_key,
  agent_key,
  agent_prompt,
  created_at,
  updated_at
FROM automation_triggered_jobs
WHERE job_key = ?
)sql";

constexpr std::string_view kListTriggeredJobsSql = R"sql(
SELECT
  job_key,
  trigger_key,
  agent_key,
  agent_prompt,
  created_at,
  updated_at
FROM automation_triggered_jobs
WHERE trigger_key = ?
ORDER BY updated_at DESC, job_key ASC
LIMIT ?
)sql";

constexpr std::string_view kRecordTriggeredRunSql = R"sql(
INSERT INTO automation_triggered_runs(
  job_key,
  trigger_key,
  fired_at,
  finished_at,
  success,
  outcome,
  error_message,
  created_at
) VALUES (?, ?, ?, ?, ?, ?, ?, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
RETURNING
  id,
  job_key,
  trigger_key,
  fired_at,
  finished_at,
  success,
  outcome,
  error_message,
  created_at
)sql";

constexpr std::string_view kListTriggeredRunsSql = R"sql(
SELECT
  id,
  job_key,
  trigger_key,
  fired_at,
  finished_at,
  success,
  outcome,
  error_message,
  created_at
FROM automation_triggered_runs
WHERE job_key = ?
ORDER BY fired_at DESC, id DESC
LIMIT ?
)sql";

constexpr std::string_view kRecordCronRunSql = R"sql(
INSERT INTO automation_cron_runs(
  job_key,
  fired_at,
  finished_at,
  success,
  outcome,
  error_message,
  created_at
) VALUES (?, ?, ?, ?, ?, ?, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
RETURNING
  id,
  job_key,
  fired_at,
  finished_at,
  success,
  outcome,
  error_message,
  created_at
)sql";

constexpr std::string_view kListCronRunsSql = R"sql(
SELECT
  id,
  job_key,
  fired_at,
  finished_at,
  success,
  outcome,
  error_message,
  created_at
FROM automation_cron_runs
WHERE job_key = ?
ORDER BY fired_at DESC, id DESC
LIMIT ?
)sql";

constexpr std::string_view kAcquireCronLeaseSql = R"sql(
INSERT INTO automation_cron_leases(
  job_key,
  owner_key,
  acquired_at,
  expires_at,
  updated_at
) VALUES (?, ?, ?, ?, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
ON CONFLICT(job_key) DO UPDATE SET
  owner_key = excluded.owner_key,
  acquired_at = excluded.acquired_at,
  expires_at = excluded.expires_at,
  updated_at = excluded.updated_at
WHERE automation_cron_leases.expires_at <= excluded.acquired_at
RETURNING
  job_key,
  owner_key,
  acquired_at,
  expires_at,
  updated_at
)sql";

constexpr std::string_view kReleaseCronLeaseSql = R"sql(
DELETE FROM automation_cron_leases
WHERE job_key = ? AND owner_key = ?
RETURNING
  job_key
)sql";

constexpr std::string_view kAcquireCronAgentLeaseSql = R"sql(
INSERT INTO automation_cron_agent_leases(
  agent_key,
  owner_key,
  acquired_at,
  expires_at,
  updated_at
) VALUES (?, ?, ?, ?, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
ON CONFLICT(agent_key) DO UPDATE SET
  owner_key = excluded.owner_key,
  acquired_at = excluded.acquired_at,
  expires_at = excluded.expires_at,
  updated_at = excluded.updated_at
WHERE automation_cron_agent_leases.expires_at <= excluded.acquired_at
RETURNING
  agent_key,
  owner_key,
  acquired_at,
  expires_at,
  updated_at
)sql";

constexpr std::string_view kReleaseCronAgentLeaseSql = R"sql(
DELETE FROM automation_cron_agent_leases
WHERE agent_key = ? AND owner_key = ?
RETURNING
  agent_key
)sql";

constexpr std::string_view kAcquireTriggeredAgentLeaseSql = R"sql(
INSERT INTO automation_triggered_agent_leases(
  agent_key,
  owner_key,
  acquired_at,
  expires_at,
  updated_at
) VALUES (?, ?, ?, ?, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
ON CONFLICT(agent_key) DO UPDATE SET
  owner_key = excluded.owner_key,
  acquired_at = excluded.acquired_at,
  expires_at = excluded.expires_at,
  updated_at = excluded.updated_at
WHERE automation_triggered_agent_leases.expires_at <= excluded.acquired_at
RETURNING
  agent_key,
  owner_key,
  acquired_at,
  expires_at,
  updated_at
)sql";

constexpr std::string_view kReleaseTriggeredAgentLeaseSql = R"sql(
DELETE FROM automation_triggered_agent_leases
WHERE agent_key = ? AND owner_key = ?
RETURNING
  agent_key
)sql";

constexpr std::string_view kUpsertMemoryRetentionJobSql = R"sql(
INSERT INTO automation_memory_retention_jobs(
  job_key,
  scope_key,
  forget_after_unused_days,
  importance_floor,
  max_records_per_scope,
  decay_check_interval_hours,
  first_fire_at,
  last_fired_at,
  created_at,
  updated_at
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
ON CONFLICT(job_key) DO UPDATE SET
  scope_key = excluded.scope_key,
  forget_after_unused_days = excluded.forget_after_unused_days,
  importance_floor = excluded.importance_floor,
  max_records_per_scope = excluded.max_records_per_scope,
  decay_check_interval_hours = excluded.decay_check_interval_hours,
  first_fire_at = excluded.first_fire_at,
  last_fired_at = excluded.last_fired_at,
  updated_at = excluded.updated_at
RETURNING
  job_key,
  scope_key,
  forget_after_unused_days,
  importance_floor,
  max_records_per_scope,
  decay_check_interval_hours,
  first_fire_at,
  last_fired_at,
  created_at,
  updated_at
)sql";

constexpr std::string_view kGetMemoryRetentionJobSql = R"sql(
SELECT
  job_key,
  scope_key,
  forget_after_unused_days,
  importance_floor,
  max_records_per_scope,
  decay_check_interval_hours,
  first_fire_at,
  last_fired_at,
  created_at,
  updated_at
FROM automation_memory_retention_jobs
WHERE job_key = ?
)sql";

constexpr std::string_view kMarkMemoryRetentionFiredSql = R"sql(
UPDATE automation_memory_retention_jobs
SET last_fired_at = ?,
    updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
WHERE job_key = ?
RETURNING
  job_key,
  scope_key,
  forget_after_unused_days,
  importance_floor,
  max_records_per_scope,
  decay_check_interval_hours,
  first_fire_at,
  last_fired_at,
  created_at,
  updated_at
)sql";

constexpr std::string_view kRecordMemoryRetentionRunSql = R"sql(
INSERT INTO automation_memory_retention_runs(
  job_key,
  fired_at,
  finished_at,
  success,
  shadowed_count,
  error_message,
  created_at
) VALUES (?, ?, ?, ?, ?, ?, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
RETURNING
  id,
  job_key,
  fired_at,
  finished_at,
  success,
  shadowed_count,
  error_message,
  created_at
)sql";

constexpr std::string_view kListMemoryRetentionRunsSql = R"sql(
SELECT
  id,
  job_key,
  fired_at,
  finished_at,
  success,
  shadowed_count,
  error_message,
  created_at
FROM automation_memory_retention_runs
WHERE job_key = ?
ORDER BY fired_at DESC, id DESC
LIMIT ?
)sql";

constexpr std::string_view kAcquireMemoryRetentionLeaseSql = R"sql(
INSERT INTO automation_memory_retention_leases(
  job_key,
  owner_key,
  acquired_at,
  expires_at,
  updated_at
) VALUES (?, ?, ?, ?, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
ON CONFLICT(job_key) DO UPDATE SET
  owner_key = excluded.owner_key,
  acquired_at = excluded.acquired_at,
  expires_at = excluded.expires_at,
  updated_at = excluded.updated_at
WHERE automation_memory_retention_leases.expires_at <= excluded.acquired_at
RETURNING
  job_key,
  owner_key,
  acquired_at,
  expires_at,
  updated_at
)sql";

constexpr std::string_view kReleaseMemoryRetentionLeaseSql = R"sql(
DELETE FROM automation_memory_retention_leases
WHERE job_key = ? AND owner_key = ?
RETURNING
  job_key
)sql";

template <std::size_t N>
[[nodiscard]] std::string to_sql_string(const unsigned char (&bytes)[N]) {
  return std::string{reinterpret_cast<const char*>(bytes), N};
}

[[nodiscard]] std::span<const storage::Migration> built_in_automation_migrations() {
  static const std::array<storage::Migration, 10> kMigrations{
      storage::Migration{
          .version = 1,
          .name = "automation-retention-state",
          .sql = to_sql_string(kAutomationRetentionStateBytes),
      },
      storage::Migration{
          .version = 2,
          .name = "automation-retention-leases",
          .sql = to_sql_string(kAutomationRetentionLeasesBytes),
      },
      storage::Migration{
          .version = 3,
          .name = "automation-cron-jobs",
          .sql = to_sql_string(kAutomationCronJobsBytes),
      },
      storage::Migration{
          .version = 4,
          .name = "automation-cron-runs",
          .sql = to_sql_string(kAutomationCronRunsBytes),
      },
      storage::Migration{
          .version = 5,
          .name = "automation-cron-run-outcomes",
          .sql = to_sql_string(kAutomationCronRunOutcomesBytes),
      },
      storage::Migration{
          .version = 6,
          .name = "automation-cron-leases",
          .sql = to_sql_string(kAutomationCronLeasesBytes),
      },
      storage::Migration{
          .version = 7,
          .name = "automation-cron-agent-leases",
          .sql = to_sql_string(kAutomationCronAgentLeasesBytes),
      },
      storage::Migration{
          .version = 8,
          .name = "automation-triggered-jobs",
          .sql = to_sql_string(kAutomationTriggeredJobsBytes),
      },
      storage::Migration{
          .version = 9,
          .name = "automation-triggered-runs",
          .sql = to_sql_string(kAutomationTriggeredRunsBytes),
      },
      storage::Migration{
          .version = 10,
          .name = "automation-triggered-agent-leases",
          .sql = to_sql_string(kAutomationTriggeredAgentLeasesBytes),
      },
  };
  return kMigrations;
}

[[nodiscard]] core::Error invalid_field(std::string field, std::string reason) {
  return core::Error::invalid_argument("automation repository field is invalid")
      .with("field", std::move(field))
      .with("reason", std::move(reason));
}

[[nodiscard]] core::Result<std::int64_t> checked_positive_i64(std::int64_t value, std::string field) {
  if (value <= 0) {
    return std::unexpected(invalid_field(std::move(field), "not_positive"));
  }
  return value;
}

[[nodiscard]] core::Result<std::int64_t> checked_non_negative_size(std::size_t value, std::string field) {
  if (value > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
    return std::unexpected(invalid_field(std::move(field), "too_large"));
  }
  return static_cast<std::int64_t>(value);
}

[[nodiscard]] core::Result<std::int64_t> checked_positive_size(std::size_t value, std::string field) {
  if (value == 0) {
    return std::unexpected(invalid_field(std::move(field), "not_positive"));
  }
  return checked_non_negative_size(value, std::move(field));
}

[[nodiscard]] core::Result<void> validate_job_key(std::string_view job_key) {
  if (job_key.empty()) {
    return std::unexpected(invalid_field("job_key", "empty"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_agent_key(std::string_view agent_key) {
  if (agent_key.empty()) {
    return std::unexpected(invalid_field("agent_key", "empty"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_agent_prompt(std::string_view agent_prompt) {
  if (agent_prompt.empty()) {
    return std::unexpected(invalid_field("agent_prompt", "empty"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_trigger_key(std::string_view trigger_key) {
  if (trigger_key.empty()) {
    return std::unexpected(invalid_field("trigger_key", "empty"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_cron_schedule_for_storage(const CronSchedule& schedule) {
  auto evaluated = evaluate_cron_schedule(schedule, {}, schedule.first_fire_at);
  if (!evaluated) {
    return std::unexpected(std::move(evaluated).error());
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_upsert_request(const UpsertCronJobRequest& request) {
  if (auto valid = validate_job_key(request.job_key); !valid) {
    return std::unexpected(valid.error());
  }
  if (auto valid = validate_agent_key(request.agent_key); !valid) {
    return std::unexpected(valid.error());
  }
  if (auto valid = validate_agent_prompt(request.agent_prompt); !valid) {
    return std::unexpected(valid.error());
  }
  if (auto valid = validate_cron_schedule_for_storage(request.schedule); !valid) {
    return std::unexpected(valid.error());
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_upsert_request(const UpsertTriggeredJobRequest& request) {
  if (auto valid = validate_job_key(request.job_key); !valid) {
    return std::unexpected(valid.error());
  }
  if (auto valid = validate_trigger_key(request.trigger_key); !valid) {
    return std::unexpected(valid.error());
  }
  if (auto valid = validate_agent_key(request.agent_key); !valid) {
    return std::unexpected(valid.error());
  }
  if (auto valid = validate_agent_prompt(request.agent_prompt); !valid) {
    return std::unexpected(valid.error());
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_cron_run_request(const RecordCronRunRequest& request) {
  if (auto valid = validate_job_key(request.job_key); !valid) {
    return std::unexpected(valid.error());
  }
  if (request.finished_at < request.fired_at) {
    return std::unexpected(invalid_field("finished_at", "before_fired_at"));
  }
  if (request.outcome != CronRunOutcome::success &&
      (!request.error_message.has_value() || request.error_message->empty())) {
    return std::unexpected(invalid_field("error_message", "missing_for_failure"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_job(const MemoryRetentionJob& job) {
  auto plan = plan_memory_retention(job, {}, job.first_fire_at);
  if (!plan) {
    return std::unexpected(std::move(plan).error());
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_upsert_request(const UpsertMemoryRetentionJobRequest& request) {
  if (auto valid = validate_job_key(request.job_key); !valid) {
    return std::unexpected(valid.error());
  }
  if (auto valid = validate_job(request.job); !valid) {
    return std::unexpected(valid.error());
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_run_request(const RecordMemoryRetentionRunRequest& request) {
  if (auto valid = validate_job_key(request.job_key); !valid) {
    return std::unexpected(valid.error());
  }
  if (request.finished_at < request.fired_at) {
    return std::unexpected(invalid_field("finished_at", "before_fired_at"));
  }
  if (!request.success && (!request.error_message.has_value() || request.error_message->empty())) {
    return std::unexpected(invalid_field("error_message", "missing_for_failure"));
  }
  if (auto count = checked_non_negative_size(request.shadowed_count, "shadowed_count"); !count) {
    return std::unexpected(count.error());
  }
  return {};
}

[[nodiscard]] core::Result<std::int64_t> checked_limit(std::size_t limit) {
  return checked_positive_size(limit, "limit");
}

[[nodiscard]] core::Result<void> validate_list_triggered_jobs_options(const ListTriggeredJobsOptions& options) {
  if (auto valid = validate_trigger_key(options.trigger_key); !valid) {
    return std::unexpected(valid.error());
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_triggered_run_request(const RecordTriggeredRunRequest& request) {
  if (auto valid = validate_job_key(request.job_key); !valid) {
    return std::unexpected(valid.error());
  }
  if (auto valid = validate_trigger_key(request.trigger_key); !valid) {
    return std::unexpected(valid.error());
  }
  if (request.finished_at < request.fired_at) {
    return std::unexpected(invalid_field("finished_at", "before_fired_at"));
  }
  if (request.outcome != TriggeredRunOutcome::success &&
      (!request.error_message.has_value() || request.error_message->empty())) {
    return std::unexpected(invalid_field("error_message", "missing_for_failure"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_owner_key(std::string_view owner_key) {
  if (owner_key.empty()) {
    return std::unexpected(invalid_field("owner_key", "empty"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_lease_request(const AcquireCronLeaseRequest& request) {
  if (auto valid = validate_job_key(request.job_key); !valid) {
    return std::unexpected(valid.error());
  }
  if (auto valid = validate_owner_key(request.owner_key); !valid) {
    return std::unexpected(valid.error());
  }
  if (request.expires_at <= request.acquired_at) {
    return std::unexpected(invalid_field("expires_at", "not_after_acquired_at"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_lease_request(const AcquireMemoryRetentionLeaseRequest& request) {
  if (auto valid = validate_job_key(request.job_key); !valid) {
    return std::unexpected(valid.error());
  }
  if (auto valid = validate_owner_key(request.owner_key); !valid) {
    return std::unexpected(valid.error());
  }
  if (request.expires_at <= request.acquired_at) {
    return std::unexpected(invalid_field("expires_at", "not_after_acquired_at"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_lease_request(const AcquireCronAgentLeaseRequest& request) {
  if (auto valid = validate_agent_key(request.agent_key); !valid) {
    return std::unexpected(valid.error());
  }
  if (auto valid = validate_owner_key(request.owner_key); !valid) {
    return std::unexpected(valid.error());
  }
  if (request.expires_at <= request.acquired_at) {
    return std::unexpected(invalid_field("expires_at", "not_after_acquired_at"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_lease_request(const AcquireTriggeredAgentLeaseRequest& request) {
  if (auto valid = validate_agent_key(request.agent_key); !valid) {
    return std::unexpected(valid.error());
  }
  if (auto valid = validate_owner_key(request.owner_key); !valid) {
    return std::unexpected(valid.error());
  }
  if (request.expires_at <= request.acquired_at) {
    return std::unexpected(invalid_field("expires_at", "not_after_acquired_at"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_release_request(const ReleaseCronLeaseRequest& request) {
  if (auto valid = validate_job_key(request.job_key); !valid) {
    return std::unexpected(valid.error());
  }
  if (auto valid = validate_owner_key(request.owner_key); !valid) {
    return std::unexpected(valid.error());
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_release_request(const ReleaseCronAgentLeaseRequest& request) {
  if (auto valid = validate_agent_key(request.agent_key); !valid) {
    return std::unexpected(valid.error());
  }
  if (auto valid = validate_owner_key(request.owner_key); !valid) {
    return std::unexpected(valid.error());
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_release_request(const ReleaseTriggeredAgentLeaseRequest& request) {
  if (auto valid = validate_agent_key(request.agent_key); !valid) {
    return std::unexpected(valid.error());
  }
  if (auto valid = validate_owner_key(request.owner_key); !valid) {
    return std::unexpected(valid.error());
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_release_request(const ReleaseMemoryRetentionLeaseRequest& request) {
  if (auto valid = validate_job_key(request.job_key); !valid) {
    return std::unexpected(valid.error());
  }
  if (auto valid = validate_owner_key(request.owner_key); !valid) {
    return std::unexpected(valid.error());
  }
  return {};
}

[[nodiscard]] core::Result<std::string>
required_text(storage::Statement& statement, int index, std::string_view field) {
  auto value = statement.column_text(index);
  if (!value) {
    return std::unexpected(value.error().with("field", std::string{field}));
  }
  if (!*value) {
    return std::unexpected(
        core::Error::storage("automation repository row has null required field").with("field", std::string{field}));
  }
  return **std::move(value);
}

[[nodiscard]] core::Result<std::optional<std::string>> optional_text(storage::Statement& statement, int index) {
  auto value = statement.column_text(index);
  if (!value) {
    return std::unexpected(value.error());
  }
  if (!*value) {
    return std::optional<std::string>{};
  }
  return std::optional<std::string>{**std::move(value)};
}

[[nodiscard]] core::Result<core::Time> required_time(storage::Statement& statement, int index, std::string_view field) {
  auto text = required_text(statement, index, field);
  if (!text) {
    return std::unexpected(text.error());
  }
  auto parsed = core::time::parse_iso8601_utc(*text);
  if (!parsed) {
    return std::unexpected(parsed.error().with("field", std::string{field}));
  }
  return *parsed;
}

[[nodiscard]] core::Result<std::optional<core::Time>> optional_time(storage::Statement& statement, int index) {
  auto text = optional_text(statement, index);
  if (!text) {
    return std::unexpected(text.error());
  }
  if (!text->has_value()) {
    return std::optional<core::Time>{};
  }
  auto parsed = core::time::parse_iso8601_utc(**text);
  if (!parsed) {
    return std::unexpected(parsed.error().with("field", "last_fired_at"));
  }
  return std::optional<core::Time>{*parsed};
}

[[nodiscard]] core::Result<std::size_t>
read_size(storage::Statement& statement, int index, std::string_view field, bool positive) {
  auto value = statement.column_int64(index);
  if (!value) {
    return std::unexpected(value.error().with("field", std::string{field}));
  }
  if ((positive && *value <= 0) || (!positive && *value < 0)) {
    return std::unexpected(core::Error::storage("automation repository row has invalid integer field")
                               .with("field", std::string{field})
                               .with("value", std::to_string(*value)));
  }
  if (static_cast<std::uint64_t>(*value) > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return std::unexpected(core::Error::storage("automation repository integer field exceeds size_t")
                               .with("field", std::string{field})
                               .with("value", std::to_string(*value)));
  }
  return static_cast<std::size_t>(*value);
}

[[nodiscard]] core::Result<void> expect_done(storage::Statement& statement, std::string_view operation) {
  auto done = statement.step();
  if (!done) {
    return std::unexpected(done.error());
  }
  if (*done != storage::StepResult::done) {
    return std::unexpected(core::Error::storage("automation repository statement returned extra rows")
                               .with("operation", std::string{operation}));
  }
  return {};
}

[[nodiscard]] bool cron_run_success(CronRunOutcome outcome) noexcept {
  return outcome == CronRunOutcome::success;
}

[[nodiscard]] bool triggered_run_success(TriggeredRunOutcome outcome) noexcept {
  return outcome == TriggeredRunOutcome::success;
}

[[nodiscard]] core::Result<TriggeredRunOutcome> read_triggered_run_outcome(storage::Statement& statement, int index) {
  auto outcome_text = required_text(statement, index, "outcome");
  if (!outcome_text) {
    return std::unexpected(outcome_text.error());
  }
  auto outcome = core::parse_enum<TriggeredRunOutcome>(*outcome_text);
  if (!outcome) {
    return std::unexpected(core::Error::storage("automation repository row has invalid triggered run outcome")
                               .with("field", "outcome")
                               .with("outcome", *outcome_text));
  }
  return *outcome;
}

[[nodiscard]] core::Result<CronRunOutcome> read_cron_run_outcome(storage::Statement& statement, int index) {
  auto outcome_text = required_text(statement, index, "outcome");
  if (!outcome_text) {
    return std::unexpected(outcome_text.error());
  }
  auto outcome = core::parse_enum<CronRunOutcome>(*outcome_text);
  if (!outcome) {
    return std::unexpected(core::Error::storage("automation repository row has invalid cron run outcome")
                               .with("field", "outcome")
                               .with("outcome", *outcome_text));
  }
  return *outcome;
}

[[nodiscard]] core::Result<MemoryRetentionJobRecord> read_job_row(storage::Statement& statement) {
  auto job_key = required_text(statement, 0, "job_key");
  if (!job_key) {
    return std::unexpected(job_key.error());
  }
  auto scope_key = required_text(statement, 1, "scope_key");
  if (!scope_key) {
    return std::unexpected(scope_key.error());
  }
  auto forget_after_unused_days = statement.column_int64(2);
  if (!forget_after_unused_days) {
    return std::unexpected(forget_after_unused_days.error().with("field", "forget_after_unused_days"));
  }
  auto importance_floor = statement.column_double(3);
  if (!importance_floor) {
    return std::unexpected(importance_floor.error().with("field", "importance_floor"));
  }
  auto max_records = read_size(statement, 4, "max_records_per_scope", true);
  if (!max_records) {
    return std::unexpected(max_records.error());
  }
  auto decay_check_interval_hours = statement.column_int64(5);
  if (!decay_check_interval_hours) {
    return std::unexpected(decay_check_interval_hours.error().with("field", "decay_check_interval_hours"));
  }
  auto first_fire_at = required_time(statement, 6, "first_fire_at");
  if (!first_fire_at) {
    return std::unexpected(first_fire_at.error());
  }
  auto last_fired_at = optional_time(statement, 7);
  if (!last_fired_at) {
    return std::unexpected(last_fired_at.error());
  }
  auto created_at = required_text(statement, 8, "created_at");
  if (!created_at) {
    return std::unexpected(created_at.error());
  }
  auto updated_at = required_text(statement, 9, "updated_at");
  if (!updated_at) {
    return std::unexpected(updated_at.error());
  }

  const auto forget_after = checked_positive_i64(*forget_after_unused_days, "forget_after_unused_days");
  if (!forget_after) {
    return std::unexpected(forget_after.error());
  }
  const auto interval = checked_positive_i64(*decay_check_interval_hours, "decay_check_interval_hours");
  if (!interval) {
    return std::unexpected(interval.error());
  }

  return MemoryRetentionJobRecord{
      .job_key = std::move(*job_key),
      .job =
          MemoryRetentionJob{
              .scope_key = std::move(*scope_key),
              .policy =
                  LongtermMemoryRetentionPolicy{
                      .forget_after_unused = std::chrono::days{*forget_after},
                      .importance_floor = *importance_floor,
                      .max_records_per_scope = *max_records,
                      .decay_check_interval = std::chrono::hours{*interval},
                  },
              .first_fire_at = *first_fire_at,
          },
      .state = PeriodicJobState{.last_fired_at = *last_fired_at},
      .created_at = std::move(*created_at),
      .updated_at = std::move(*updated_at),
  };
}

[[nodiscard]] core::Result<CronJobRecord> read_cron_job_row(storage::Statement& statement) {
  auto job_key = required_text(statement, 0, "job_key");
  if (!job_key) {
    return std::unexpected(job_key.error());
  }
  auto agent_key = required_text(statement, 1, "agent_key");
  if (!agent_key) {
    return std::unexpected(agent_key.error());
  }
  if (auto valid = validate_agent_key(*agent_key); !valid) {
    return std::unexpected(valid.error());
  }
  auto agent_prompt = required_text(statement, 2, "agent_prompt");
  if (!agent_prompt) {
    return std::unexpected(agent_prompt.error());
  }
  if (auto valid = validate_agent_prompt(*agent_prompt); !valid) {
    return std::unexpected(valid.error());
  }
  auto expression = required_text(statement, 3, "expression");
  if (!expression) {
    return std::unexpected(expression.error());
  }
  auto first_fire_at = required_time(statement, 4, "first_fire_at");
  if (!first_fire_at) {
    return std::unexpected(first_fire_at.error());
  }
  auto last_fired_at = optional_time(statement, 5);
  if (!last_fired_at) {
    return std::unexpected(last_fired_at.error());
  }
  auto created_at = required_text(statement, 6, "created_at");
  if (!created_at) {
    return std::unexpected(created_at.error());
  }
  auto updated_at = required_text(statement, 7, "updated_at");
  if (!updated_at) {
    return std::unexpected(updated_at.error());
  }

  auto schedule = CronSchedule{
      .expression = std::move(*expression),
      .first_fire_at = *first_fire_at,
  };
  if (auto valid = validate_cron_schedule_for_storage(schedule); !valid) {
    return std::unexpected(valid.error());
  }

  return CronJobRecord{
      .job_key = std::move(*job_key),
      .agent_key = std::move(*agent_key),
      .agent_prompt = std::move(*agent_prompt),
      .schedule = std::move(schedule),
      .state = PeriodicJobState{.last_fired_at = *last_fired_at},
      .created_at = std::move(*created_at),
      .updated_at = std::move(*updated_at),
  };
}

[[nodiscard]] core::Result<TriggeredJobRecord> read_triggered_job_row(storage::Statement& statement) {
  auto job_key = required_text(statement, 0, "job_key");
  if (!job_key) {
    return std::unexpected(job_key.error());
  }
  auto trigger_key = required_text(statement, 1, "trigger_key");
  if (!trigger_key) {
    return std::unexpected(trigger_key.error());
  }
  if (auto valid = validate_trigger_key(*trigger_key); !valid) {
    return std::unexpected(valid.error());
  }
  auto agent_key = required_text(statement, 2, "agent_key");
  if (!agent_key) {
    return std::unexpected(agent_key.error());
  }
  if (auto valid = validate_agent_key(*agent_key); !valid) {
    return std::unexpected(valid.error());
  }
  auto agent_prompt = required_text(statement, 3, "agent_prompt");
  if (!agent_prompt) {
    return std::unexpected(agent_prompt.error());
  }
  if (auto valid = validate_agent_prompt(*agent_prompt); !valid) {
    return std::unexpected(valid.error());
  }
  auto created_at = required_text(statement, 4, "created_at");
  if (!created_at) {
    return std::unexpected(created_at.error());
  }
  auto updated_at = required_text(statement, 5, "updated_at");
  if (!updated_at) {
    return std::unexpected(updated_at.error());
  }

  return TriggeredJobRecord{
      .job_key = std::move(*job_key),
      .trigger_key = std::move(*trigger_key),
      .agent_key = std::move(*agent_key),
      .agent_prompt = std::move(*agent_prompt),
      .created_at = std::move(*created_at),
      .updated_at = std::move(*updated_at),
  };
}

[[nodiscard]] core::Result<TriggeredRunRecord> read_triggered_run_row(storage::Statement& statement) {
  auto id = statement.column_int64(0);
  if (!id) {
    return std::unexpected(id.error().with("field", "id"));
  }
  auto job_key = required_text(statement, 1, "job_key");
  if (!job_key) {
    return std::unexpected(job_key.error());
  }
  auto trigger_key = required_text(statement, 2, "trigger_key");
  if (!trigger_key) {
    return std::unexpected(trigger_key.error());
  }
  if (auto valid = validate_trigger_key(*trigger_key); !valid) {
    return std::unexpected(valid.error());
  }
  auto fired_at = required_time(statement, 3, "fired_at");
  if (!fired_at) {
    return std::unexpected(fired_at.error());
  }
  auto finished_at = required_time(statement, 4, "finished_at");
  if (!finished_at) {
    return std::unexpected(finished_at.error());
  }
  auto success = statement.column_int64(5);
  if (!success) {
    return std::unexpected(success.error().with("field", "success"));
  }
  if (*success != 0 && *success != 1) {
    return std::unexpected(core::Error::storage("automation repository row has invalid success value")
                               .with("success", std::to_string(*success)));
  }
  auto outcome = read_triggered_run_outcome(statement, 6);
  if (!outcome) {
    return std::unexpected(outcome.error());
  }
  if ((*success == 1) != triggered_run_success(*outcome)) {
    return std::unexpected(
        core::Error::storage("automation repository row has mismatched triggered run success outcome")
            .with("success", std::to_string(*success))
            .with("outcome", std::string{core::enum_name(*outcome)}));
  }
  auto error_message = optional_text(statement, 7);
  if (!error_message) {
    return std::unexpected(error_message.error());
  }
  auto created_at = required_text(statement, 8, "created_at");
  if (!created_at) {
    return std::unexpected(created_at.error());
  }

  return TriggeredRunRecord{
      .id = *id,
      .job_key = std::move(*job_key),
      .trigger_key = std::move(*trigger_key),
      .fired_at = *fired_at,
      .finished_at = *finished_at,
      .success = *success == 1,
      .outcome = *outcome,
      .error_message = std::move(*error_message),
      .created_at = std::move(*created_at),
  };
}

[[nodiscard]] core::Result<MemoryRetentionRunRecord> read_run_row(storage::Statement& statement) {
  auto id = statement.column_int64(0);
  if (!id) {
    return std::unexpected(id.error().with("field", "id"));
  }
  auto job_key = required_text(statement, 1, "job_key");
  if (!job_key) {
    return std::unexpected(job_key.error());
  }
  auto fired_at = required_time(statement, 2, "fired_at");
  if (!fired_at) {
    return std::unexpected(fired_at.error());
  }
  auto finished_at = required_time(statement, 3, "finished_at");
  if (!finished_at) {
    return std::unexpected(finished_at.error());
  }
  auto success = statement.column_int64(4);
  if (!success) {
    return std::unexpected(success.error().with("field", "success"));
  }
  if (*success != 0 && *success != 1) {
    return std::unexpected(core::Error::storage("automation repository row has invalid success value")
                               .with("success", std::to_string(*success)));
  }
  auto shadowed_count = read_size(statement, 5, "shadowed_count", false);
  if (!shadowed_count) {
    return std::unexpected(shadowed_count.error());
  }
  auto error_message = optional_text(statement, 6);
  if (!error_message) {
    return std::unexpected(error_message.error());
  }
  auto created_at = required_text(statement, 7, "created_at");
  if (!created_at) {
    return std::unexpected(created_at.error());
  }

  return MemoryRetentionRunRecord{
      .id = *id,
      .job_key = std::move(*job_key),
      .fired_at = *fired_at,
      .finished_at = *finished_at,
      .success = *success == 1,
      .shadowed_count = *shadowed_count,
      .error_message = std::move(*error_message),
      .created_at = std::move(*created_at),
  };
}

[[nodiscard]] core::Result<CronRunRecord> read_cron_run_row(storage::Statement& statement) {
  auto id = statement.column_int64(0);
  if (!id) {
    return std::unexpected(id.error().with("field", "id"));
  }
  auto job_key = required_text(statement, 1, "job_key");
  if (!job_key) {
    return std::unexpected(job_key.error());
  }
  auto fired_at = required_time(statement, 2, "fired_at");
  if (!fired_at) {
    return std::unexpected(fired_at.error());
  }
  auto finished_at = required_time(statement, 3, "finished_at");
  if (!finished_at) {
    return std::unexpected(finished_at.error());
  }
  auto success = statement.column_int64(4);
  if (!success) {
    return std::unexpected(success.error().with("field", "success"));
  }
  if (*success != 0 && *success != 1) {
    return std::unexpected(core::Error::storage("automation repository row has invalid success value")
                               .with("success", std::to_string(*success)));
  }
  auto outcome = read_cron_run_outcome(statement, 5);
  if (!outcome) {
    return std::unexpected(outcome.error());
  }
  if ((*success == 1) != cron_run_success(*outcome)) {
    return std::unexpected(core::Error::storage("automation repository row has mismatched cron run success outcome")
                               .with("success", std::to_string(*success))
                               .with("outcome", std::string{core::enum_name(*outcome)}));
  }
  auto error_message = optional_text(statement, 6);
  if (!error_message) {
    return std::unexpected(error_message.error());
  }
  auto created_at = required_text(statement, 7, "created_at");
  if (!created_at) {
    return std::unexpected(created_at.error());
  }

  return CronRunRecord{
      .id = *id,
      .job_key = std::move(*job_key),
      .fired_at = *fired_at,
      .finished_at = *finished_at,
      .success = *success == 1,
      .outcome = *outcome,
      .error_message = std::move(*error_message),
      .created_at = std::move(*created_at),
  };
}

template <typename LeaseRecord>
[[nodiscard]] core::Result<LeaseRecord> read_lease_row(storage::Statement& statement) {
  auto job_key = required_text(statement, 0, "job_key");
  if (!job_key) {
    return std::unexpected(job_key.error());
  }
  auto owner_key = required_text(statement, 1, "owner_key");
  if (!owner_key) {
    return std::unexpected(owner_key.error());
  }
  auto acquired_at = required_time(statement, 2, "acquired_at");
  if (!acquired_at) {
    return std::unexpected(acquired_at.error());
  }
  auto expires_at = required_time(statement, 3, "expires_at");
  if (!expires_at) {
    return std::unexpected(expires_at.error());
  }
  auto updated_at = required_text(statement, 4, "updated_at");
  if (!updated_at) {
    return std::unexpected(updated_at.error());
  }
  if (*expires_at <= *acquired_at) {
    return std::unexpected(
        core::Error::storage("automation repository row has invalid lease expiry").with("field", "expires_at"));
  }

  return LeaseRecord{
      .job_key = std::move(*job_key),
      .owner_key = std::move(*owner_key),
      .acquired_at = *acquired_at,
      .expires_at = *expires_at,
      .updated_at = std::move(*updated_at),
  };
}

template <typename AgentLeaseRecord>
[[nodiscard]] core::Result<AgentLeaseRecord> read_agent_lease_row(storage::Statement& statement) {
  auto agent_key = required_text(statement, 0, "agent_key");
  if (!agent_key) {
    return std::unexpected(agent_key.error());
  }
  auto owner_key = required_text(statement, 1, "owner_key");
  if (!owner_key) {
    return std::unexpected(owner_key.error());
  }
  auto acquired_at = required_time(statement, 2, "acquired_at");
  if (!acquired_at) {
    return std::unexpected(acquired_at.error());
  }
  auto expires_at = required_time(statement, 3, "expires_at");
  if (!expires_at) {
    return std::unexpected(expires_at.error());
  }
  auto updated_at = required_text(statement, 4, "updated_at");
  if (!updated_at) {
    return std::unexpected(updated_at.error());
  }
  if (*expires_at <= *acquired_at) {
    return std::unexpected(
        core::Error::storage("automation repository row has invalid lease expiry").with("field", "expires_at"));
  }

  return AgentLeaseRecord{
      .agent_key = std::move(*agent_key),
      .owner_key = std::move(*owner_key),
      .acquired_at = *acquired_at,
      .expires_at = *expires_at,
      .updated_at = std::move(*updated_at),
  };
}

[[nodiscard]] core::Result<void>
bind_optional_time(storage::Statement& statement, int index, std::optional<core::Time> value) {
  if (!value) {
    return statement.bind_null(index);
  }
  return statement.bind_text(index, core::time::format_iso8601_utc(*value));
}

[[nodiscard]] core::Result<void>
bind_optional_text(storage::Statement& statement, int index, const std::optional<std::string>& value) {
  if (!value) {
    return statement.bind_null(index);
  }
  return statement.bind_text(index, *value);
}

}  // namespace

AutomationRepository::AutomationRepository(storage::Pool& pool, AutomationRepositoryOptions options) noexcept
    : pool_{&pool}, options_{std::move(options)} {}

async::Awaitable<core::Result<storage::MigrationReport>> AutomationRepository::migrate() {
  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(writer.error());
  }

  if (options_.migrations_directory.empty()) {
    auto report = storage::run_migrations(writer->connection(), built_in_automation_migrations());
    if (!report) {
      co_return std::unexpected(report.error());
    }
    co_return std::move(*report);
  }

  auto report = storage::run_migrations_from_directory(writer->connection(), options_.migrations_directory);
  if (!report) {
    co_return std::unexpected(report.error());
  }
  co_return std::move(*report);
}

async::Awaitable<core::Result<CronJobRecord>> AutomationRepository::upsert_cron_job(UpsertCronJobRequest request) {
  if (auto valid = validate_upsert_request(request); !valid) {
    co_return std::unexpected(valid.error());
  }

  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(writer.error());
  }

  auto cached = writer->statement_cache().acquire(writer->connection(), kUpsertCronJobSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_text(1, request.job_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(2, request.agent_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(3, request.agent_prompt); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(4, request.schedule.expression); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(5, core::time::format_iso8601_utc(request.schedule.first_fire_at)); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = bind_optional_time(statement, 6, request.state.last_fired_at); !bound) {
    co_return std::unexpected(bound.error());
  }

  auto step = statement.step();
  if (!step) {
    co_return std::unexpected(step.error());
  }
  if (*step != storage::StepResult::row) {
    co_return std::unexpected(core::Error::storage("automation cron job upsert returned no row"));
  }

  auto record = read_cron_job_row(statement);
  if (!record) {
    co_return std::unexpected(record.error());
  }
  if (auto done = expect_done(statement, "upsert_cron_job"); !done) {
    co_return std::unexpected(done.error());
  }
  co_return std::move(*record);
}

async::Awaitable<core::Result<std::optional<CronJobRecord>>> AutomationRepository::get_cron_job(std::string job_key) {
  if (auto valid = validate_job_key(job_key); !valid) {
    co_return std::unexpected(valid.error());
  }

  auto reader = co_await pool_->acquire_reader();
  if (!reader) {
    co_return std::unexpected(reader.error());
  }

  auto cached = reader->statement_cache().acquire(reader->connection(), kGetCronJobSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_text(1, job_key); !bound) {
    co_return std::unexpected(bound.error());
  }

  auto step = statement.step();
  if (!step) {
    co_return std::unexpected(step.error());
  }
  if (*step == storage::StepResult::done) {
    co_return std::optional<CronJobRecord>{};
  }
  auto record = read_cron_job_row(statement);
  if (!record) {
    co_return std::unexpected(record.error());
  }
  if (auto done = expect_done(statement, "get_cron_job"); !done) {
    co_return std::unexpected(done.error());
  }
  co_return std::optional<CronJobRecord>{std::move(*record)};
}

async::Awaitable<core::Result<CronJobRecord>> AutomationRepository::mark_cron_job_fired(std::string job_key,
                                                                                        core::Time fired_at) {
  if (auto valid = validate_job_key(job_key); !valid) {
    co_return std::unexpected(valid.error());
  }

  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(writer.error());
  }

  auto cached = writer->statement_cache().acquire(writer->connection(), kMarkCronJobFiredSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_text(1, core::time::format_iso8601_utc(fired_at)); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(2, job_key); !bound) {
    co_return std::unexpected(bound.error());
  }

  auto step = statement.step();
  if (!step) {
    co_return std::unexpected(step.error());
  }
  if (*step == storage::StepResult::done) {
    co_return std::unexpected(core::Error::not_found("automation cron job not found").with("job_key", job_key));
  }
  auto record = read_cron_job_row(statement);
  if (!record) {
    co_return std::unexpected(record.error());
  }
  if (auto done = expect_done(statement, "mark_cron_job_fired"); !done) {
    co_return std::unexpected(done.error());
  }
  co_return std::move(*record);
}

async::Awaitable<core::Result<std::vector<CronJobRecord>>>
AutomationRepository::list_cron_jobs(ListCronJobsOptions options) {
  auto limit = checked_limit(options.limit);
  if (!limit) {
    co_return std::unexpected(limit.error());
  }

  auto reader = co_await pool_->acquire_reader();
  if (!reader) {
    co_return std::unexpected(reader.error());
  }

  auto cached = reader->statement_cache().acquire(reader->connection(), kListCronJobsSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_int64(1, *limit); !bound) {
    co_return std::unexpected(bound.error());
  }

  std::vector<CronJobRecord> rows;
  while (true) {
    auto step = statement.step();
    if (!step) {
      co_return std::unexpected(step.error());
    }
    if (*step == storage::StepResult::done) {
      break;
    }
    auto row = read_cron_job_row(statement);
    if (!row) {
      co_return std::unexpected(row.error());
    }
    rows.push_back(std::move(*row));
  }
  co_return rows;
}

async::Awaitable<core::Result<TriggeredJobRecord>>
AutomationRepository::upsert_triggered_job(UpsertTriggeredJobRequest request) {
  if (auto valid = validate_upsert_request(request); !valid) {
    co_return std::unexpected(valid.error());
  }

  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(writer.error());
  }

  auto cached = writer->statement_cache().acquire(writer->connection(), kUpsertTriggeredJobSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_text(1, request.job_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(2, request.trigger_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(3, request.agent_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(4, request.agent_prompt); !bound) {
    co_return std::unexpected(bound.error());
  }

  auto step = statement.step();
  if (!step) {
    co_return std::unexpected(step.error());
  }
  if (*step != storage::StepResult::row) {
    co_return std::unexpected(core::Error::storage("automation triggered job upsert returned no row"));
  }

  auto record = read_triggered_job_row(statement);
  if (!record) {
    co_return std::unexpected(record.error());
  }
  if (auto done = expect_done(statement, "upsert_triggered_job"); !done) {
    co_return std::unexpected(done.error());
  }
  co_return std::move(*record);
}

async::Awaitable<core::Result<std::optional<TriggeredJobRecord>>>
AutomationRepository::get_triggered_job(std::string job_key) {
  if (auto valid = validate_job_key(job_key); !valid) {
    co_return std::unexpected(valid.error());
  }

  auto reader = co_await pool_->acquire_reader();
  if (!reader) {
    co_return std::unexpected(reader.error());
  }

  auto cached = reader->statement_cache().acquire(reader->connection(), kGetTriggeredJobSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_text(1, job_key); !bound) {
    co_return std::unexpected(bound.error());
  }

  auto step = statement.step();
  if (!step) {
    co_return std::unexpected(step.error());
  }
  if (*step == storage::StepResult::done) {
    co_return std::optional<TriggeredJobRecord>{};
  }
  auto record = read_triggered_job_row(statement);
  if (!record) {
    co_return std::unexpected(record.error());
  }
  if (auto done = expect_done(statement, "get_triggered_job"); !done) {
    co_return std::unexpected(done.error());
  }
  co_return std::optional<TriggeredJobRecord>{std::move(*record)};
}

async::Awaitable<core::Result<std::vector<TriggeredJobRecord>>>
AutomationRepository::list_triggered_jobs(ListTriggeredJobsOptions options) {
  if (auto valid = validate_list_triggered_jobs_options(options); !valid) {
    co_return std::unexpected(valid.error());
  }
  auto limit = checked_limit(options.limit);
  if (!limit) {
    co_return std::unexpected(limit.error());
  }

  auto reader = co_await pool_->acquire_reader();
  if (!reader) {
    co_return std::unexpected(reader.error());
  }

  auto cached = reader->statement_cache().acquire(reader->connection(), kListTriggeredJobsSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_text(1, options.trigger_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_int64(2, *limit); !bound) {
    co_return std::unexpected(bound.error());
  }

  std::vector<TriggeredJobRecord> rows;
  while (true) {
    auto step = statement.step();
    if (!step) {
      co_return std::unexpected(step.error());
    }
    if (*step == storage::StepResult::done) {
      break;
    }
    auto row = read_triggered_job_row(statement);
    if (!row) {
      co_return std::unexpected(row.error());
    }
    rows.push_back(std::move(*row));
  }
  co_return rows;
}

async::Awaitable<core::Result<TriggeredRunRecord>>
AutomationRepository::record_triggered_run(RecordTriggeredRunRequest request) {
  if (auto valid = validate_triggered_run_request(request); !valid) {
    co_return std::unexpected(valid.error());
  }

  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(writer.error());
  }

  auto cached = writer->statement_cache().acquire(writer->connection(), kRecordTriggeredRunSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_text(1, request.job_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(2, request.trigger_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(3, core::time::format_iso8601_utc(request.fired_at)); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(4, core::time::format_iso8601_utc(request.finished_at)); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_int64(5, triggered_run_success(request.outcome) ? 1 : 0); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(6, core::enum_name(request.outcome)); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = bind_optional_text(statement, 7, request.error_message); !bound) {
    co_return std::unexpected(bound.error());
  }

  auto step = statement.step();
  if (!step) {
    co_return std::unexpected(step.error());
  }
  if (*step != storage::StepResult::row) {
    co_return std::unexpected(core::Error::storage("automation triggered run insert returned no row"));
  }
  auto record = read_triggered_run_row(statement);
  if (!record) {
    co_return std::unexpected(record.error());
  }
  if (auto done = expect_done(statement, "record_triggered_run"); !done) {
    co_return std::unexpected(done.error());
  }
  co_return std::move(*record);
}

async::Awaitable<core::Result<std::vector<TriggeredRunRecord>>>
AutomationRepository::list_triggered_runs(ListTriggeredRunsOptions options) {
  if (auto valid = validate_job_key(options.job_key); !valid) {
    co_return std::unexpected(valid.error());
  }
  auto limit = checked_limit(options.limit);
  if (!limit) {
    co_return std::unexpected(limit.error());
  }

  auto reader = co_await pool_->acquire_reader();
  if (!reader) {
    co_return std::unexpected(reader.error());
  }

  auto cached = reader->statement_cache().acquire(reader->connection(), kListTriggeredRunsSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_text(1, options.job_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_int64(2, *limit); !bound) {
    co_return std::unexpected(bound.error());
  }

  std::vector<TriggeredRunRecord> rows;
  while (true) {
    auto step = statement.step();
    if (!step) {
      co_return std::unexpected(step.error());
    }
    if (*step == storage::StepResult::done) {
      break;
    }
    auto row = read_triggered_run_row(statement);
    if (!row) {
      co_return std::unexpected(row.error());
    }
    rows.push_back(std::move(*row));
  }
  co_return rows;
}

async::Awaitable<core::Result<CronRunRecord>> AutomationRepository::record_cron_run(RecordCronRunRequest request) {
  if (auto valid = validate_cron_run_request(request); !valid) {
    co_return std::unexpected(valid.error());
  }

  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(writer.error());
  }

  auto cached = writer->statement_cache().acquire(writer->connection(), kRecordCronRunSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_text(1, request.job_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(2, core::time::format_iso8601_utc(request.fired_at)); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(3, core::time::format_iso8601_utc(request.finished_at)); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_int64(4, cron_run_success(request.outcome) ? 1 : 0); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(5, core::enum_name(request.outcome)); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = bind_optional_text(statement, 6, request.error_message); !bound) {
    co_return std::unexpected(bound.error());
  }

  auto step = statement.step();
  if (!step) {
    co_return std::unexpected(step.error());
  }
  if (*step != storage::StepResult::row) {
    co_return std::unexpected(core::Error::storage("automation cron run insert returned no row"));
  }
  auto record = read_cron_run_row(statement);
  if (!record) {
    co_return std::unexpected(record.error());
  }
  if (auto done = expect_done(statement, "record_cron_run"); !done) {
    co_return std::unexpected(done.error());
  }
  co_return std::move(*record);
}

async::Awaitable<core::Result<std::vector<CronRunRecord>>>
AutomationRepository::list_cron_runs(ListCronRunsOptions options) {
  if (auto valid = validate_job_key(options.job_key); !valid) {
    co_return std::unexpected(valid.error());
  }
  auto limit = checked_limit(options.limit);
  if (!limit) {
    co_return std::unexpected(limit.error());
  }

  auto reader = co_await pool_->acquire_reader();
  if (!reader) {
    co_return std::unexpected(reader.error());
  }

  auto cached = reader->statement_cache().acquire(reader->connection(), kListCronRunsSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_text(1, options.job_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_int64(2, *limit); !bound) {
    co_return std::unexpected(bound.error());
  }

  std::vector<CronRunRecord> rows;
  while (true) {
    auto step = statement.step();
    if (!step) {
      co_return std::unexpected(step.error());
    }
    if (*step == storage::StepResult::done) {
      break;
    }
    auto row = read_cron_run_row(statement);
    if (!row) {
      co_return std::unexpected(row.error());
    }
    rows.push_back(std::move(*row));
  }
  co_return rows;
}

async::Awaitable<core::Result<MemoryRetentionJobRecord>>
AutomationRepository::upsert_memory_retention_job(UpsertMemoryRetentionJobRequest request) {
  if (auto valid = validate_upsert_request(request); !valid) {
    co_return std::unexpected(valid.error());
  }
  auto max_records = checked_positive_size(request.job.policy.max_records_per_scope, "max_records_per_scope");
  if (!max_records) {
    co_return std::unexpected(max_records.error());
  }

  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(writer.error());
  }

  auto cached = writer->statement_cache().acquire(writer->connection(), kUpsertMemoryRetentionJobSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_text(1, request.job_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(2, request.job.scope_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_int64(3, request.job.policy.forget_after_unused.count()); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_double(4, request.job.policy.importance_floor); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_int64(5, *max_records); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_int64(6, request.job.policy.decay_check_interval.count()); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(7, core::time::format_iso8601_utc(request.job.first_fire_at)); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = bind_optional_time(statement, 8, request.state.last_fired_at); !bound) {
    co_return std::unexpected(bound.error());
  }

  auto step = statement.step();
  if (!step) {
    co_return std::unexpected(step.error());
  }
  if (*step != storage::StepResult::row) {
    co_return std::unexpected(core::Error::storage("automation memory-retention job upsert returned no row"));
  }

  auto record = read_job_row(statement);
  if (!record) {
    co_return std::unexpected(record.error());
  }
  if (auto done = expect_done(statement, "upsert_memory_retention_job"); !done) {
    co_return std::unexpected(done.error());
  }
  co_return std::move(*record);
}

async::Awaitable<core::Result<std::optional<MemoryRetentionJobRecord>>>
AutomationRepository::get_memory_retention_job(std::string job_key) {
  if (auto valid = validate_job_key(job_key); !valid) {
    co_return std::unexpected(valid.error());
  }

  auto reader = co_await pool_->acquire_reader();
  if (!reader) {
    co_return std::unexpected(reader.error());
  }

  auto cached = reader->statement_cache().acquire(reader->connection(), kGetMemoryRetentionJobSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_text(1, job_key); !bound) {
    co_return std::unexpected(bound.error());
  }

  auto step = statement.step();
  if (!step) {
    co_return std::unexpected(step.error());
  }
  if (*step == storage::StepResult::done) {
    co_return std::optional<MemoryRetentionJobRecord>{};
  }
  auto record = read_job_row(statement);
  if (!record) {
    co_return std::unexpected(record.error());
  }
  if (auto done = expect_done(statement, "get_memory_retention_job"); !done) {
    co_return std::unexpected(done.error());
  }
  co_return std::optional<MemoryRetentionJobRecord>{std::move(*record)};
}

async::Awaitable<core::Result<MemoryRetentionJobRecord>>
AutomationRepository::mark_memory_retention_fired(std::string job_key, core::Time fired_at) {
  if (auto valid = validate_job_key(job_key); !valid) {
    co_return std::unexpected(valid.error());
  }

  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(writer.error());
  }

  auto cached = writer->statement_cache().acquire(writer->connection(), kMarkMemoryRetentionFiredSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_text(1, core::time::format_iso8601_utc(fired_at)); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(2, job_key); !bound) {
    co_return std::unexpected(bound.error());
  }

  auto step = statement.step();
  if (!step) {
    co_return std::unexpected(step.error());
  }
  if (*step == storage::StepResult::done) {
    co_return std::unexpected(
        core::Error::not_found("automation memory-retention job not found").with("job_key", job_key));
  }
  auto record = read_job_row(statement);
  if (!record) {
    co_return std::unexpected(record.error());
  }
  if (auto done = expect_done(statement, "mark_memory_retention_fired"); !done) {
    co_return std::unexpected(done.error());
  }
  co_return std::move(*record);
}

async::Awaitable<core::Result<MemoryRetentionRunRecord>>
AutomationRepository::record_memory_retention_run(RecordMemoryRetentionRunRequest request) {
  if (auto valid = validate_run_request(request); !valid) {
    co_return std::unexpected(valid.error());
  }
  auto shadowed_count = checked_non_negative_size(request.shadowed_count, "shadowed_count");
  if (!shadowed_count) {
    co_return std::unexpected(shadowed_count.error());
  }

  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(writer.error());
  }

  auto cached = writer->statement_cache().acquire(writer->connection(), kRecordMemoryRetentionRunSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_text(1, request.job_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(2, core::time::format_iso8601_utc(request.fired_at)); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(3, core::time::format_iso8601_utc(request.finished_at)); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_int64(4, request.success ? 1 : 0); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_int64(5, *shadowed_count); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = bind_optional_text(statement, 6, request.error_message); !bound) {
    co_return std::unexpected(bound.error());
  }

  auto step = statement.step();
  if (!step) {
    co_return std::unexpected(step.error());
  }
  if (*step != storage::StepResult::row) {
    co_return std::unexpected(core::Error::storage("automation memory-retention run insert returned no row"));
  }
  auto record = read_run_row(statement);
  if (!record) {
    co_return std::unexpected(record.error());
  }
  if (auto done = expect_done(statement, "record_memory_retention_run"); !done) {
    co_return std::unexpected(done.error());
  }
  co_return std::move(*record);
}

async::Awaitable<core::Result<std::vector<MemoryRetentionRunRecord>>>
AutomationRepository::list_memory_retention_runs(ListMemoryRetentionRunsOptions options) {
  if (auto valid = validate_job_key(options.job_key); !valid) {
    co_return std::unexpected(valid.error());
  }
  auto limit = checked_limit(options.limit);
  if (!limit) {
    co_return std::unexpected(limit.error());
  }

  auto reader = co_await pool_->acquire_reader();
  if (!reader) {
    co_return std::unexpected(reader.error());
  }

  auto cached = reader->statement_cache().acquire(reader->connection(), kListMemoryRetentionRunsSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_text(1, options.job_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_int64(2, *limit); !bound) {
    co_return std::unexpected(bound.error());
  }

  std::vector<MemoryRetentionRunRecord> rows;
  while (true) {
    auto step = statement.step();
    if (!step) {
      co_return std::unexpected(step.error());
    }
    if (*step == storage::StepResult::done) {
      break;
    }
    auto row = read_run_row(statement);
    if (!row) {
      co_return std::unexpected(row.error());
    }
    rows.push_back(std::move(*row));
  }
  co_return rows;
}

async::Awaitable<core::Result<std::optional<CronLeaseRecord>>>
AutomationRepository::acquire_cron_lease(AcquireCronLeaseRequest request) {
  if (auto valid = validate_lease_request(request); !valid) {
    co_return std::unexpected(valid.error());
  }

  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(writer.error());
  }

  auto job_statement = writer->statement_cache().acquire(writer->connection(), kGetCronJobSql);
  if (!job_statement) {
    co_return std::unexpected(job_statement.error());
  }
  auto& job_lookup = job_statement->statement();
  if (auto bound = job_lookup.bind_text(1, request.job_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  auto job_step = job_lookup.step();
  if (!job_step) {
    co_return std::unexpected(job_step.error());
  }
  if (*job_step == storage::StepResult::done) {
    co_return std::unexpected(core::Error::not_found("automation cron job not found").with("job_key", request.job_key));
  }
  if (auto done = expect_done(job_lookup, "lease_get_cron_job"); !done) {
    co_return std::unexpected(done.error());
  }

  auto cached = writer->statement_cache().acquire(writer->connection(), kAcquireCronLeaseSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_text(1, request.job_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(2, request.owner_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(3, core::time::format_iso8601_utc(request.acquired_at)); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(4, core::time::format_iso8601_utc(request.expires_at)); !bound) {
    co_return std::unexpected(bound.error());
  }

  auto step = statement.step();
  if (!step) {
    co_return std::unexpected(step.error());
  }
  if (*step == storage::StepResult::row) {
    auto record = read_lease_row<CronLeaseRecord>(statement);
    if (!record) {
      co_return std::unexpected(record.error());
    }
    if (auto done = expect_done(statement, "acquire_cron_lease"); !done) {
      co_return std::unexpected(done.error());
    }
    co_return std::optional<CronLeaseRecord>{std::move(*record)};
  }
  co_return std::optional<CronLeaseRecord>{};
}

async::Awaitable<core::Result<bool>> AutomationRepository::release_cron_lease(ReleaseCronLeaseRequest request) {
  if (auto valid = validate_release_request(request); !valid) {
    co_return std::unexpected(valid.error());
  }

  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(writer.error());
  }

  auto cached = writer->statement_cache().acquire(writer->connection(), kReleaseCronLeaseSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_text(1, request.job_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(2, request.owner_key); !bound) {
    co_return std::unexpected(bound.error());
  }

  auto step = statement.step();
  if (!step) {
    co_return std::unexpected(step.error());
  }
  if (*step == storage::StepResult::done) {
    co_return false;
  }
  if (auto done = expect_done(statement, "release_cron_lease"); !done) {
    co_return std::unexpected(done.error());
  }
  co_return true;
}

async::Awaitable<core::Result<std::optional<CronAgentLeaseRecord>>>
AutomationRepository::acquire_cron_agent_lease(AcquireCronAgentLeaseRequest request) {
  if (auto valid = validate_lease_request(request); !valid) {
    co_return std::unexpected(valid.error());
  }

  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(writer.error());
  }

  auto cached = writer->statement_cache().acquire(writer->connection(), kAcquireCronAgentLeaseSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_text(1, request.agent_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(2, request.owner_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(3, core::time::format_iso8601_utc(request.acquired_at)); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(4, core::time::format_iso8601_utc(request.expires_at)); !bound) {
    co_return std::unexpected(bound.error());
  }

  auto step = statement.step();
  if (!step) {
    co_return std::unexpected(step.error());
  }
  if (*step == storage::StepResult::row) {
    auto record = read_agent_lease_row<CronAgentLeaseRecord>(statement);
    if (!record) {
      co_return std::unexpected(record.error());
    }
    if (auto done = expect_done(statement, "acquire_cron_agent_lease"); !done) {
      co_return std::unexpected(done.error());
    }
    co_return std::optional<CronAgentLeaseRecord>{std::move(*record)};
  }
  co_return std::optional<CronAgentLeaseRecord>{};
}

async::Awaitable<core::Result<bool>>
AutomationRepository::release_cron_agent_lease(ReleaseCronAgentLeaseRequest request) {
  if (auto valid = validate_release_request(request); !valid) {
    co_return std::unexpected(valid.error());
  }

  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(writer.error());
  }

  auto cached = writer->statement_cache().acquire(writer->connection(), kReleaseCronAgentLeaseSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_text(1, request.agent_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(2, request.owner_key); !bound) {
    co_return std::unexpected(bound.error());
  }

  auto step = statement.step();
  if (!step) {
    co_return std::unexpected(step.error());
  }
  if (*step == storage::StepResult::done) {
    co_return false;
  }
  if (auto done = expect_done(statement, "release_cron_agent_lease"); !done) {
    co_return std::unexpected(done.error());
  }
  co_return true;
}

async::Awaitable<core::Result<std::optional<TriggeredAgentLeaseRecord>>>
AutomationRepository::acquire_triggered_agent_lease(AcquireTriggeredAgentLeaseRequest request) {
  if (auto valid = validate_lease_request(request); !valid) {
    co_return std::unexpected(valid.error());
  }

  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(writer.error());
  }

  auto cached = writer->statement_cache().acquire(writer->connection(), kAcquireTriggeredAgentLeaseSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_text(1, request.agent_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(2, request.owner_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(3, core::time::format_iso8601_utc(request.acquired_at)); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(4, core::time::format_iso8601_utc(request.expires_at)); !bound) {
    co_return std::unexpected(bound.error());
  }

  auto step = statement.step();
  if (!step) {
    co_return std::unexpected(step.error());
  }
  if (*step == storage::StepResult::row) {
    auto record = read_agent_lease_row<TriggeredAgentLeaseRecord>(statement);
    if (!record) {
      co_return std::unexpected(record.error());
    }
    if (auto done = expect_done(statement, "acquire_triggered_agent_lease"); !done) {
      co_return std::unexpected(done.error());
    }
    co_return std::optional<TriggeredAgentLeaseRecord>{std::move(*record)};
  }
  co_return std::optional<TriggeredAgentLeaseRecord>{};
}

async::Awaitable<core::Result<bool>>
AutomationRepository::release_triggered_agent_lease(ReleaseTriggeredAgentLeaseRequest request) {
  if (auto valid = validate_release_request(request); !valid) {
    co_return std::unexpected(valid.error());
  }

  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(writer.error());
  }

  auto cached = writer->statement_cache().acquire(writer->connection(), kReleaseTriggeredAgentLeaseSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_text(1, request.agent_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(2, request.owner_key); !bound) {
    co_return std::unexpected(bound.error());
  }

  auto step = statement.step();
  if (!step) {
    co_return std::unexpected(step.error());
  }
  if (*step == storage::StepResult::done) {
    co_return false;
  }
  if (auto done = expect_done(statement, "release_triggered_agent_lease"); !done) {
    co_return std::unexpected(done.error());
  }
  co_return true;
}

async::Awaitable<core::Result<std::optional<MemoryRetentionLeaseRecord>>>
AutomationRepository::acquire_memory_retention_lease(AcquireMemoryRetentionLeaseRequest request) {
  if (auto valid = validate_lease_request(request); !valid) {
    co_return std::unexpected(valid.error());
  }

  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(writer.error());
  }

  auto job_statement = writer->statement_cache().acquire(writer->connection(), kGetMemoryRetentionJobSql);
  if (!job_statement) {
    co_return std::unexpected(job_statement.error());
  }
  auto& job_lookup = job_statement->statement();
  if (auto bound = job_lookup.bind_text(1, request.job_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  auto job_step = job_lookup.step();
  if (!job_step) {
    co_return std::unexpected(job_step.error());
  }
  if (*job_step == storage::StepResult::done) {
    co_return std::unexpected(
        core::Error::not_found("automation memory-retention job not found").with("job_key", request.job_key));
  }
  if (auto done = expect_done(job_lookup, "lease_get_memory_retention_job"); !done) {
    co_return std::unexpected(done.error());
  }

  auto cached = writer->statement_cache().acquire(writer->connection(), kAcquireMemoryRetentionLeaseSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_text(1, request.job_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(2, request.owner_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(3, core::time::format_iso8601_utc(request.acquired_at)); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(4, core::time::format_iso8601_utc(request.expires_at)); !bound) {
    co_return std::unexpected(bound.error());
  }

  auto step = statement.step();
  if (!step) {
    co_return std::unexpected(step.error());
  }
  if (*step == storage::StepResult::row) {
    auto record = read_lease_row<MemoryRetentionLeaseRecord>(statement);
    if (!record) {
      co_return std::unexpected(record.error());
    }
    if (auto done = expect_done(statement, "acquire_memory_retention_lease"); !done) {
      co_return std::unexpected(done.error());
    }
    co_return std::optional<MemoryRetentionLeaseRecord>{std::move(*record)};
  }
  co_return std::optional<MemoryRetentionLeaseRecord>{};
}

async::Awaitable<core::Result<bool>>
AutomationRepository::release_memory_retention_lease(ReleaseMemoryRetentionLeaseRequest request) {
  if (auto valid = validate_release_request(request); !valid) {
    co_return std::unexpected(valid.error());
  }

  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(writer.error());
  }

  auto cached = writer->statement_cache().acquire(writer->connection(), kReleaseMemoryRetentionLeaseSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_text(1, request.job_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(2, request.owner_key); !bound) {
    co_return std::unexpected(bound.error());
  }

  auto step = statement.step();
  if (!step) {
    co_return std::unexpected(step.error());
  }
  if (*step == storage::StepResult::done) {
    co_return false;
  }
  if (auto done = expect_done(statement, "release_memory_retention_lease"); !done) {
    co_return std::unexpected(done.error());
  }
  co_return true;
}

}  // namespace orangutan::automation
