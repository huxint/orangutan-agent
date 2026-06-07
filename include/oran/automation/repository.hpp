// include/oran/automation/repository.hpp - automation persistence boundary.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/automation/periodic.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>
#include <oran/storage/migrations.hpp>

namespace orangutan::storage {
class Pool;
}  // namespace orangutan::storage

namespace orangutan::automation {

struct AutomationRepositoryOptions {
  std::string migrations_directory;
};

struct UpsertCronJobRequest {
  std::string job_key;
  CronSchedule schedule;
  PeriodicJobState state{};
};

struct CronJobRecord {
  std::string job_key;
  CronSchedule schedule;
  PeriodicJobState state{};
  std::string created_at;
  std::string updated_at;
};

struct ListCronJobsOptions {
  std::size_t limit{50};
};

struct RecordCronRunRequest {
  std::string job_key;
  core::Time fired_at{core::Time::epoch()};
  core::Time finished_at{core::Time::epoch()};
  bool success{true};
  std::optional<std::string> error_message{};
};

struct CronRunRecord {
  std::int64_t id{};
  std::string job_key;
  core::Time fired_at{core::Time::epoch()};
  core::Time finished_at{core::Time::epoch()};
  bool success{true};
  std::optional<std::string> error_message{};
  std::string created_at;
};

struct ListCronRunsOptions {
  std::string job_key;
  std::size_t limit{50};
};

struct UpsertMemoryRetentionJobRequest {
  std::string job_key;
  MemoryRetentionJob job;
  PeriodicJobState state{};
};

struct MemoryRetentionJobRecord {
  std::string job_key;
  MemoryRetentionJob job;
  PeriodicJobState state{};
  std::string created_at;
  std::string updated_at;
};

struct RecordMemoryRetentionRunRequest {
  std::string job_key;
  core::Time fired_at{core::Time::epoch()};
  core::Time finished_at{core::Time::epoch()};
  bool success{true};
  std::size_t shadowed_count{0};
  std::optional<std::string> error_message{};
};

struct MemoryRetentionRunRecord {
  std::int64_t id{};
  std::string job_key;
  core::Time fired_at{core::Time::epoch()};
  core::Time finished_at{core::Time::epoch()};
  bool success{true};
  std::size_t shadowed_count{0};
  std::optional<std::string> error_message{};
  std::string created_at;
};

struct ListMemoryRetentionRunsOptions {
  std::string job_key;
  std::size_t limit{50};
};

struct AcquireMemoryRetentionLeaseRequest {
  std::string job_key;
  std::string owner_key;
  core::Time acquired_at{core::Time::epoch()};
  core::Time expires_at{core::Time::epoch()};
};

struct MemoryRetentionLeaseRecord {
  std::string job_key;
  std::string owner_key;
  core::Time acquired_at{core::Time::epoch()};
  core::Time expires_at{core::Time::epoch()};
  std::string updated_at;
};

struct ReleaseMemoryRetentionLeaseRequest {
  std::string job_key;
  std::string owner_key;
};

class AutomationRepository {
public:
  explicit AutomationRepository(storage::Pool& pool, AutomationRepositoryOptions options = {}) noexcept;

  [[nodiscard]] async::Awaitable<core::Result<storage::MigrationReport>> migrate();

  [[nodiscard]] async::Awaitable<core::Result<CronJobRecord>> upsert_cron_job(UpsertCronJobRequest request);

  [[nodiscard]] async::Awaitable<core::Result<std::optional<CronJobRecord>>> get_cron_job(std::string job_key);

  [[nodiscard]] async::Awaitable<core::Result<CronJobRecord>> mark_cron_job_fired(std::string job_key,
                                                                                  core::Time fired_at);

  [[nodiscard]] async::Awaitable<core::Result<std::vector<CronJobRecord>>>
  list_cron_jobs(ListCronJobsOptions options = {});

  [[nodiscard]] async::Awaitable<core::Result<CronRunRecord>> record_cron_run(RecordCronRunRequest request);

  [[nodiscard]] async::Awaitable<core::Result<std::vector<CronRunRecord>>> list_cron_runs(ListCronRunsOptions options);

  [[nodiscard]] async::Awaitable<core::Result<MemoryRetentionJobRecord>>
  upsert_memory_retention_job(UpsertMemoryRetentionJobRequest request);

  [[nodiscard]] async::Awaitable<core::Result<std::optional<MemoryRetentionJobRecord>>>
  get_memory_retention_job(std::string job_key);

  [[nodiscard]] async::Awaitable<core::Result<MemoryRetentionJobRecord>>
  mark_memory_retention_fired(std::string job_key, core::Time fired_at);

  [[nodiscard]] async::Awaitable<core::Result<MemoryRetentionRunRecord>>
  record_memory_retention_run(RecordMemoryRetentionRunRequest request);

  [[nodiscard]] async::Awaitable<core::Result<std::vector<MemoryRetentionRunRecord>>>
  list_memory_retention_runs(ListMemoryRetentionRunsOptions options);

  [[nodiscard]] async::Awaitable<core::Result<std::optional<MemoryRetentionLeaseRecord>>>
  acquire_memory_retention_lease(AcquireMemoryRetentionLeaseRequest request);

  [[nodiscard]] async::Awaitable<core::Result<bool>>
  release_memory_retention_lease(ReleaseMemoryRetentionLeaseRequest request);

private:
  storage::Pool* pool_{};
  AutomationRepositoryOptions options_;
};

}  // namespace orangutan::automation
