// src/oran-automation/runtime.cpp - caller-owned automation state handle.

#include <oran/automation/runtime.hpp>

#include <chrono>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

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

}  // namespace

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

async::Awaitable<core::Result<CronSeedApplyResult>>
AutomationRuntime::apply_cron_job_seeds(std::vector<UpsertCronJobRequest> seeds) {
  auto result = CronSeedApplyResult{
      .requested_count = seeds.size(),
  };
  result.jobs.reserve(seeds.size());

  std::size_t index = 0;
  for (auto& seed : seeds) {
    auto job_key = seed.job_key;
    auto upserted = co_await impl_->repository.upsert_cron_job(std::move(seed));
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

async::Awaitable<core::Result<CronServiceCycleResult>>
AutomationRuntime::run_cron_service_cycle(CronServiceCycleRequest request) {
  if (auto valid = validate_cron_service_cycle_request(request); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }

  auto applied = co_await apply_cron_job_seeds(std::move(request.seeds));
  if (!applied) {
    co_return std::unexpected(std::move(applied).error());
  }

  auto loop = cron_loop(std::move(request.service_options));
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

CronService AutomationRuntime::cron_service(CronServiceOptions options) noexcept {
  return CronService{impl_->repository, std::move(options)};
}

CronLoop AutomationRuntime::cron_loop(CronServiceOptions options) noexcept {
  return CronLoop{impl_->executor, cron_service(std::move(options))};
}

TriggeredService AutomationRuntime::triggered_service(TriggeredServiceOptions options) noexcept {
  return TriggeredService{impl_->repository, std::move(options)};
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
