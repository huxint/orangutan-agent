// src/oran-automation/runtime.cpp - caller-owned automation state handle.

#include <oran/automation/runtime.hpp>

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
  Impl(std::string path, storage::Pool opened_pool, AutomationRepositoryOptions repository_options)
      : database_path{std::move(path)}, pool{std::move(opened_pool)}, repository{pool, std::move(repository_options)} {}

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

  auto impl = std::make_unique<Impl>(std::move(options.database_path), std::move(*pool), std::move(options.repository));
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

MemoryRetentionService AutomationRuntime::memory_retention_service(memory::longterm::Backend& backend,
                                                                   MemoryRetentionServiceOptions options) noexcept {
  return MemoryRetentionService{impl_->repository, backend, std::move(options)};
}

}  // namespace orangutan::automation
