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

template <std::size_t N>
[[nodiscard]] std::string to_sql_string(const unsigned char (&bytes)[N]) {
  return std::string{reinterpret_cast<const char*>(bytes), N};
}

[[nodiscard]] std::span<const storage::Migration> built_in_automation_migrations() {
  static const std::array<storage::Migration, 1> kMigrations{
      storage::Migration{
          .version = 1,
          .name = "automation-retention-state",
          .sql = to_sql_string(kAutomationRetentionStateBytes),
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

}  // namespace orangutan::automation
