// src/oran-automation/runtime.cpp - caller-owned automation state handle.

#include <oran/automation/runtime.hpp>

#include <chrono>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/storage/pool.hpp>

namespace orangutan::automation {
namespace {

[[nodiscard]] core::Error invalid_runtime_field(std::string field, std::string reason) {
  return core::Error::invalid_argument("automation runtime field is invalid")
      .with("field", std::move(field))
      .with("reason", std::move(reason));
}

[[nodiscard]] core::Result<void> validate_open_options(const AutomationRuntimeOptions& options) {
  if (options.database_path.empty()) {
    return std::unexpected(invalid_runtime_field("database_path", "empty"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_cron_service_cycle_request(const CronServiceCycleRequest& request) {
  if (request.max_total_wait < std::chrono::steady_clock::duration::zero()) {
    return std::unexpected(invalid_runtime_field("max_total_wait", "negative"));
  }
  if (request.max_iterations == 0) {
    return std::unexpected(invalid_runtime_field("max_iterations", "zero"));
  }
  if (request.job_limit == 0) {
    return std::unexpected(invalid_runtime_field("job_limit", "zero"));
  }
  if (!request.handler) {
    return std::unexpected(invalid_runtime_field("handler", "empty"));
  }
  if (request.lease_owner_key.empty()) {
    return std::unexpected(invalid_runtime_field("lease_owner_key", "empty"));
  }
  if (request.lease_ttl <= std::chrono::steady_clock::duration::zero()) {
    return std::unexpected(invalid_runtime_field("lease_ttl", "not_positive"));
  }
  return {};
}

[[nodiscard]] core::Result<void>
validate_automation_service_cycle_request(const AutomationServiceCycleRequest& request) {
  if (request.max_total_wait < std::chrono::steady_clock::duration::zero()) {
    return std::unexpected(invalid_runtime_field("max_total_wait", "negative"));
  }
  if (request.max_iterations == 0) {
    return std::unexpected(invalid_runtime_field("max_iterations", "zero"));
  }
  if (request.cron_job_limit == 0) {
    return std::unexpected(invalid_runtime_field("cron_job_limit", "zero"));
  }
  if (!request.cron_handler) {
    return std::unexpected(invalid_runtime_field("cron_handler", "empty"));
  }
  if (request.cron_lease_owner_key.empty()) {
    return std::unexpected(invalid_runtime_field("cron_lease_owner_key", "empty"));
  }
  if (request.cron_lease_ttl <= std::chrono::steady_clock::duration::zero()) {
    return std::unexpected(invalid_runtime_field("cron_lease_ttl", "not_positive"));
  }
  if (!request.triggered_handler) {
    return std::unexpected(invalid_runtime_field("triggered_handler", "empty"));
  }
  if (request.triggered_max_jobs == 0) {
    return std::unexpected(invalid_runtime_field("triggered_max_jobs", "zero"));
  }
  if (!request.triggered_lease_owner_key.empty() &&
      request.triggered_lease_ttl <= std::chrono::steady_clock::duration::zero()) {
    return std::unexpected(invalid_runtime_field("triggered_lease_ttl", "not_positive"));
  }
  if (core::enum_name(request.blocked_agent_policy) == "unknown") {
    return std::unexpected(invalid_runtime_field("blocked_agent_policy", "unknown"));
  }
  return {};
}

[[nodiscard]] core::Result<void> ensure_parent_directory(const std::filesystem::path& target) {
  const auto parent = target.parent_path();
  if (parent.empty()) {
    return {};
  }

  std::error_code ec;
  std::filesystem::create_directories(parent, ec);
  if (ec) {
    return std::unexpected(core::Error::io("failed to create automation database directory")
                               .with("path", parent.string())
                               .with("reason", ec.message()));
  }
  return {};
}

async::Awaitable<core::Result<CronSeedApplyResult>> apply_cron_job_seeds_impl(AutomationRepository& repository,
                                                                              std::vector<UpsertCronJobRequest> seeds) {
  auto result = CronSeedApplyResult{
      .requested_count = seeds.size(),
  };
  result.jobs.reserve(seeds.size());

  std::size_t index = 0;
  for (auto& seed : seeds) {
    auto job_key = seed.job_key;
    auto upserted = co_await repository.upsert_cron_job(std::move(seed));
    if (!upserted) {
      auto error = std::move(upserted.error());
      error.with("seed_index", std::to_string(index));
      if (!job_key.empty()) {
        error.with("job_key", std::move(job_key));
      }
      co_return std::unexpected(std::move(error));
    }
    result.jobs.push_back(std::move(*upserted));
    ++result.upserted_count;
    ++index;
  }

  co_return result;
}

async::Awaitable<core::Result<CronServiceCycleResult>> run_cron_service_cycle_impl(asio::any_io_executor executor,
                                                                                   AutomationRepository& repository,
                                                                                   CronServiceCycleRequest request) {
  if (auto valid = validate_cron_service_cycle_request(request); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }

  auto applied = co_await apply_cron_job_seeds_impl(repository, std::move(request.seeds));
  if (!applied) {
    co_return std::unexpected(std::move(applied).error());
  }

  auto loop = CronLoop{std::move(executor), CronService{repository, std::move(request.service_options)}};
  auto loop_result = co_await loop.run(CronLoopRunRequest{
      .now = request.now,
      .max_total_wait = request.max_total_wait,
      .max_iterations = request.max_iterations,
      .job_limit = request.job_limit,
      .handler = std::move(request.handler),
      .lease_owner_key = std::move(request.lease_owner_key),
      .lease_ttl = request.lease_ttl,
      .stop_requested = std::move(request.stop_requested),
  });
  if (!loop_result) {
    co_return std::unexpected(std::move(loop_result).error());
  }

  co_return CronServiceCycleResult{
      .seed_apply = std::move(*applied),
      .loop = std::move(*loop_result),
  };
}

}  // namespace

struct AutomationService::Impl {
  Impl(asio::any_io_executor runtime_executor,
       AutomationRepository& runtime_repository,
       AutomationServiceOptions options)
      : executor{std::move(runtime_executor)}, repository{&runtime_repository}, cron_options{options.cron},
        triggered_queue{
            executor,
            TriggeredService{runtime_repository,
                             TriggeredServiceOptions{
                                 .hooks = options.triggered_queue.hooks,
                                 .notifier = options.triggered_queue.notifier,
                             }},
            std::move(options.triggered_queue),
        } {}

  asio::any_io_executor executor;
  AutomationRepository* repository;
  CronServiceOptions cron_options;
  TriggeredQueue triggered_queue;
};

struct AutomationRuntime::Impl {
  Impl(asio::any_io_executor runtime_executor,
       std::string path,
       storage::Pool opened_pool,
       AutomationRepositoryOptions repository_options)
      : executor{std::move(runtime_executor)}, database_path{std::move(path)}, pool{std::move(opened_pool)},
        repository{pool, std::move(repository_options)} {}

  asio::any_io_executor executor;
  std::string database_path;
  storage::Pool pool;
  AutomationRepository repository;
  storage::MigrationReport migration_report{};
};

AutomationRuntime::AutomationRuntime(std::unique_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}

AutomationRuntime::~AutomationRuntime() = default;

AutomationRuntime::AutomationRuntime(AutomationRuntime&&) noexcept = default;

AutomationRuntime& AutomationRuntime::operator=(AutomationRuntime&&) noexcept = default;

async::Awaitable<core::Result<AutomationRuntime>> AutomationRuntime::open(asio::any_io_executor executor,
                                                                          AutomationRuntimeOptions options) {
  if (auto valid = validate_open_options(options); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }
  if (auto parent = ensure_parent_directory(std::filesystem::path{options.database_path}); !parent) {
    co_return std::unexpected(std::move(parent).error());
  }

  auto runtime_executor = executor;
  auto pool = storage::Pool::open(std::move(executor),
                                  storage::PoolOptions{
                                      .path = options.database_path,
                                      .reader_count = options.reader_count,
                                      .statement_cache_capacity = options.statement_cache_capacity,
                                      .busy_timeout_ms = options.busy_timeout_ms,
                                      .enable_wal = options.enable_wal,
                                      .enforce_foreign_keys = options.enforce_foreign_keys,
                                  });
  if (!pool) {
    co_return std::unexpected(std::move(pool).error().with("database", "automation"));
  }

  auto impl = std::make_unique<Impl>(std::move(runtime_executor),
                                     std::move(options.database_path),
                                     std::move(*pool),
                                     std::move(options.repository));
  auto migrated = co_await impl->repository.migrate();
  if (!migrated) {
    co_return std::unexpected(std::move(migrated).error().with("database", "automation"));
  }
  impl->migration_report = std::move(*migrated);

  co_return AutomationRuntime{std::move(impl)};
}

std::string_view AutomationRuntime::database_path() const noexcept {
  return impl_->database_path;
}

const storage::MigrationReport& AutomationRuntime::migration_report() const noexcept {
  return impl_->migration_report;
}

AutomationRepository& AutomationRuntime::repository() noexcept {
  return impl_->repository;
}

const AutomationRepository& AutomationRuntime::repository() const noexcept {
  return impl_->repository;
}

AutomationService::AutomationService(std::unique_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}

AutomationService::~AutomationService() = default;

AutomationService::AutomationService(AutomationService&&) noexcept = default;

AutomationService& AutomationService::operator=(AutomationService&&) noexcept = default;

async::Awaitable<core::Result<TriggeredQueueEnqueueResult>>
AutomationService::enqueue_triggered(TriggeredQueueEnqueueRequest request) {
  co_return co_await impl_->triggered_queue.enqueue(std::move(request));
}

async::Awaitable<core::Result<AutomationServiceCycleResult>>
AutomationService::run_cycle(AutomationServiceCycleRequest request) {
  if (auto valid = validate_automation_service_cycle_request(request); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }

  auto triggered = co_await impl_->triggered_queue.drain_available(TriggeredQueueDrainAvailableRequest{
      .handler = std::move(request.triggered_handler),
      .max_jobs = request.triggered_max_jobs,
      .lease_owner_key = std::move(request.triggered_lease_owner_key),
      .lease_ttl = request.triggered_lease_ttl,
      .blocked_agent_policy = request.blocked_agent_policy,
  });
  if (!triggered) {
    co_return std::unexpected(std::move(triggered).error());
  }

  auto cron = co_await run_cron_service_cycle_impl(impl_->executor,
                                                   *impl_->repository,
                                                   CronServiceCycleRequest{
                                                       .seeds = std::move(request.cron_seeds),
                                                       .service_options = impl_->cron_options,
                                                       .now = request.now,
                                                       .max_total_wait = request.max_total_wait,
                                                       .max_iterations = request.max_iterations,
                                                       .job_limit = request.cron_job_limit,
                                                       .handler = std::move(request.cron_handler),
                                                       .lease_owner_key = std::move(request.cron_lease_owner_key),
                                                       .lease_ttl = request.cron_lease_ttl,
                                                       .stop_requested = std::move(request.stop_requested),
                                                   });
  if (!cron) {
    co_return std::unexpected(std::move(cron).error());
  }

  co_return AutomationServiceCycleResult{
      .triggered = std::move(*triggered),
      .cron = std::move(*cron),
  };
}

std::size_t AutomationService::triggered_queue_capacity() const noexcept {
  return impl_->triggered_queue.capacity();
}

std::size_t AutomationService::triggered_queue_size() const {
  return impl_->triggered_queue.size();
}

async::Awaitable<core::Result<CronSeedApplyResult>>
AutomationRuntime::apply_cron_job_seeds(std::vector<UpsertCronJobRequest> seeds) {
  co_return co_await apply_cron_job_seeds_impl(impl_->repository, std::move(seeds));
}

async::Awaitable<core::Result<CronServiceCycleResult>>
AutomationRuntime::run_cron_service_cycle(CronServiceCycleRequest request) {
  co_return co_await run_cron_service_cycle_impl(impl_->executor, impl_->repository, std::move(request));
}

AutomationService AutomationRuntime::automation_service(AutomationServiceOptions options) {
  return AutomationService{
      std::make_unique<AutomationService::Impl>(impl_->executor, impl_->repository, std::move(options))};
}

CronService AutomationRuntime::cron_service(CronServiceOptions options) noexcept {
  return CronService{impl_->repository, std::move(options)};
}

CronLoop AutomationRuntime::cron_loop(CronServiceOptions options) noexcept {
  return CronLoop{impl_->executor, cron_service(std::move(options))};
}

TriggeredService AutomationRuntime::triggered_service(TriggeredServiceOptions options) noexcept {
  return TriggeredService{impl_->repository, std::move(options)};
}

TriggeredQueue AutomationRuntime::triggered_queue(TriggeredQueueOptions options) {
  auto hooks = options.hooks;
  auto notifier = options.notifier;
  return TriggeredQueue{impl_->executor,
                        triggered_service(TriggeredServiceOptions{
                            .hooks = std::move(hooks),
                            .notifier = std::move(notifier),
                        }),
                        std::move(options)};
}

MemoryRetentionService AutomationRuntime::memory_retention_service(memory::longterm::Backend& backend,
                                                                   MemoryRetentionServiceOptions options) noexcept {
  return MemoryRetentionService{impl_->repository, backend, std::move(options)};
}

MemoryRetentionLoop AutomationRuntime::memory_retention_loop(memory::longterm::Backend& backend,
                                                             MemoryRetentionServiceOptions options) noexcept {
  return MemoryRetentionLoop{impl_->executor, memory_retention_service(backend, std::move(options))};
}

}  // namespace orangutan::automation
