// include/oran/automation/runtime.hpp - caller-owned automation state handle.

#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <asio/any_io_executor.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/automation/loop.hpp>
#include <oran/automation/service.hpp>
#include <oran/core/result.hpp>
#include <oran/storage/migrations.hpp>

namespace orangutan::automation {

struct AutomationRuntimeOptions {
  std::string database_path;
  std::size_t reader_count{2};
  std::size_t statement_cache_capacity{8};
  int busy_timeout_ms{5000};
  bool enable_wal{true};
  bool enforce_foreign_keys{true};
  AutomationRepositoryOptions repository{};
};

struct CronSeedApplyResult {
  std::size_t requested_count{0};
  std::size_t upserted_count{0};
  std::vector<CronJobRecord> jobs{};
};

struct CronServiceCycleRequest {
  std::vector<UpsertCronJobRequest> seeds{};
  CronServiceOptions service_options{};
  core::Time now{core::Time::epoch()};
  std::chrono::steady_clock::duration max_total_wait{std::chrono::steady_clock::duration::zero()};
  std::size_t max_iterations{1};
  std::size_t job_limit{100};
  CronJobHandler handler{};
  CronLoopStopPredicate stop_requested{};
};

struct CronServiceCycleResult {
  CronSeedApplyResult seed_apply{};
  CronLoopRunResult loop{};
};

/// Caller-owned automation state bundle.
///
/// Opening the runtime creates parent directories, opens `automation.db`, runs
/// the automation migrations, and keeps the pool/repository lifetime stable for
/// service owners. It can explicitly upsert caller-supplied cron seeds or run
/// one caller-awaited cron service cycle over the owned repository, but
/// intentionally does not start detached timers, acquire process ownership, or
/// launch background jobs.
class AutomationRuntime {
public:
  ~AutomationRuntime();

  AutomationRuntime(const AutomationRuntime&) = delete;
  AutomationRuntime& operator=(const AutomationRuntime&) = delete;
  AutomationRuntime(AutomationRuntime&&) noexcept;
  AutomationRuntime& operator=(AutomationRuntime&&) noexcept;

  [[nodiscard]] static async::Awaitable<core::Result<AutomationRuntime>> open(asio::any_io_executor executor,
                                                                              AutomationRuntimeOptions options);

  [[nodiscard]] std::string_view database_path() const noexcept;
  [[nodiscard]] const storage::MigrationReport& migration_report() const noexcept;
  [[nodiscard]] AutomationRepository& repository() noexcept;
  [[nodiscard]] const AutomationRepository& repository() const noexcept;

  [[nodiscard]] async::Awaitable<core::Result<CronSeedApplyResult>>
  apply_cron_job_seeds(std::vector<UpsertCronJobRequest> seeds);

  [[nodiscard]] async::Awaitable<core::Result<CronServiceCycleResult>>
  run_cron_service_cycle(CronServiceCycleRequest request);

  [[nodiscard]] CronService cron_service(CronServiceOptions options = {}) noexcept;
  [[nodiscard]] CronLoop cron_loop(CronServiceOptions options = {}) noexcept;

  [[nodiscard]] MemoryRetentionService memory_retention_service(memory::longterm::Backend& backend,
                                                                MemoryRetentionServiceOptions options = {}) noexcept;
  [[nodiscard]] MemoryRetentionLoop memory_retention_loop(memory::longterm::Backend& backend,
                                                          MemoryRetentionServiceOptions options = {}) noexcept;

private:
  struct Impl;

  explicit AutomationRuntime(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

}  // namespace orangutan::automation
