// src/oran-bootstrap/runtime_assembly.cpp — per-process permission + audit assembly.

#include <oran/bootstrap/runtime_assembly.hpp>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/any_io_executor.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <oran/async.hpp>
#include <oran/core/error.hpp>
#include <oran/permission.hpp>
#include <oran/storage.hpp>
#include <oran/tool/workspace.hpp>

namespace orangutan::bootstrap {
namespace {

using ::orangutan::core::Error;
using ::orangutan::core::Result;

constexpr std::string_view kAuditDatabaseRelative = ".orangutan/audit.db";

[[nodiscard]] std::string resolve_audit_path(std::string_view workspace, std::string_view override_path) {
  if (!override_path.empty()) {
    return std::string{override_path};
  }
  auto path = std::filesystem::path{std::string{workspace}};
  path /= kAuditDatabaseRelative;
  return path.string();
}

[[nodiscard]] Result<void> ensure_parent_directory(const std::filesystem::path& target) {
  if (auto parent = target.parent_path(); !parent.empty()) {
    auto ec = std::error_code{};
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      return std::unexpected(
          Error::io("failed to create audit directory").with("path", parent.string()).with("detail", ec.message()));
    }
  }
  return {};
}

/// Drive `repo.migrate()` to completion on a one-shot `asio::io_context`.
/// Mirrors `bootstrap::run_audit_init`'s pattern so the runtime
/// assembly and the `--audit-init` operator command remain
/// behaviourally identical at the schema-migration boundary.
[[nodiscard]] Result<storage::MigrationReport>
run_migration_inline(const std::string& audit_path, std::size_t reader_count, std::size_t statement_cache_capacity) {
  asio::io_context io;
  auto temp_pool_result = storage::Pool::open(io.get_executor(),
                                              storage::PoolOptions{
                                                  .path = audit_path,
                                                  .reader_count = reader_count,
                                                  .statement_cache_capacity = statement_cache_capacity,
                                              });
  if (!temp_pool_result) {
    return std::unexpected(std::move(temp_pool_result).error());
  }
  auto temp_pool = std::move(*temp_pool_result);
  storage::AuditRepository repo{temp_pool};

  auto report = storage::MigrationReport{};
  auto migrate_error = std::optional<Error>{};
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        auto migrated = co_await repo.migrate();
        if (!migrated) {
          migrate_error = std::move(migrated).error();
          co_return;
        }
        report = std::move(*migrated);
        co_return;
      },
      asio::detached);
  io.run();

  if (migrate_error) {
    return std::unexpected(std::move(*migrate_error));
  }
  return report;
}

}  // namespace

struct RuntimeAssembly::Impl {
  bool audit_enabled{false};
  std::string audit_path;
  // The members below are non-default-constructible in their final
  // shape and capture pointers into each other (`AuditRepository`
  // refers to `audit_pool`, `audit_sink` refers to `audit_repository`).
  // Heap-allocating each via `unique_ptr` keeps their addresses stable
  // across `RuntimeAssembly` moves: the assembly's `impl_` pointer
  // changes hands, but the Impl itself stays put on the heap.
  std::unique_ptr<storage::Pool> audit_pool;
  std::unique_ptr<storage::AuditRepository> audit_repository;
  std::unique_ptr<storage::TraceRepository> trace_repository;
  std::unique_ptr<permission::AuditSink> audit_sink;
  std::unique_ptr<permission::ApprovalBroker> approval_broker;
  std::unique_ptr<tool::Workspace> workspace;
};

RuntimeAssembly::RuntimeAssembly(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

RuntimeAssembly::RuntimeAssembly(RuntimeAssembly&&) noexcept = default;

RuntimeAssembly& RuntimeAssembly::operator=(RuntimeAssembly&&) noexcept = default;

RuntimeAssembly::~RuntimeAssembly() = default;

permission::AuditSink& RuntimeAssembly::audit_sink() noexcept {
  return *impl_->audit_sink;
}

permission::ApprovalBroker& RuntimeAssembly::approval_broker() noexcept {
  return *impl_->approval_broker;
}

bool RuntimeAssembly::audit_enabled() const noexcept {
  return impl_->audit_enabled;
}

std::string_view RuntimeAssembly::audit_path() const noexcept {
  return impl_->audit_path;
}

tool::Workspace& RuntimeAssembly::workspace() noexcept {
  return *impl_->workspace;
}

const tool::Workspace& RuntimeAssembly::workspace() const noexcept {
  return *impl_->workspace;
}

storage::TraceRepository* RuntimeAssembly::trace_repository() noexcept {
  return impl_->trace_repository.get();
}

bool RuntimeAssembly::trace_enabled() const noexcept {
  return impl_->trace_repository != nullptr;
}

Result<RuntimeAssembly> RuntimeAssembly::build(std::string_view workspace,
                                               asio::any_io_executor runtime_executor,
                                               RuntimeAssemblyOptions options) {
  if (workspace.empty()) {
    return std::unexpected(Error::invalid_argument("workspace path is empty"));
  }

  auto impl = std::make_unique<Impl>();
  impl->audit_enabled = options.audit_enabled;

  // Build the workspace resolver before audit/broker. `tool::Workspace::create`
  // validates the workspace root (must exist + be a directory) and
  // canonicalises every `extra_{read,write}_roots` entry, so a misconfigured
  // override fails the boot rather than the first dispatched tool call.
  auto workspace_result = tool::Workspace::create(workspace, std::move(options.workspace_options));
  if (!workspace_result) {
    return std::unexpected(std::move(workspace_result).error());
  }
  impl->workspace = std::make_unique<tool::Workspace>(std::move(*workspace_result));

  // ApprovalBroker is the same shape regardless of audit mode — the
  // per-process secret rotation guarantee (criterion 5) applies to every
  // runtime. Build it first so even the audit-disabled path can return a
  // usable broker.
  auto broker_result = permission::ApprovalBroker::with_random_secret();
  if (!broker_result) {
    return std::unexpected(std::move(broker_result).error());
  }
  impl->approval_broker = std::make_unique<permission::ApprovalBroker>(std::move(*broker_result));

  if (!options.audit_enabled) {
    impl->audit_sink = std::make_unique<permission::NullAuditSink>();
    return RuntimeAssembly{std::move(impl)};
  }

  impl->audit_path = resolve_audit_path(workspace, options.audit_db_path);
  if (auto parent_ok = ensure_parent_directory(std::filesystem::path{impl->audit_path}); !parent_ok) {
    return std::unexpected(std::move(parent_ok).error());
  }

  auto migration =
      run_migration_inline(impl->audit_path, options.audit_reader_count, options.audit_statement_cache_capacity);
  if (!migration) {
    return std::unexpected(std::move(migration).error());
  }

  auto long_lived_pool = storage::Pool::open(std::move(runtime_executor),
                                             storage::PoolOptions{
                                                 .path = impl->audit_path,
                                                 .reader_count = options.audit_reader_count,
                                                 .statement_cache_capacity = options.audit_statement_cache_capacity,
                                             });
  if (!long_lived_pool) {
    return std::unexpected(std::move(long_lived_pool).error());
  }
  impl->audit_pool = std::make_unique<storage::Pool>(std::move(*long_lived_pool));
  impl->audit_repository = std::make_unique<storage::AuditRepository>(*impl->audit_pool);
  impl->audit_sink = std::make_unique<permission::StorageAuditSink>(*impl->audit_repository);

  // The trace schema rides on the same audit DB migration stream (slice 78
  // pinned `built_in_trace_migrations()` to the complete audit set), so the
  // `trace_turns` table is already present at this point. Building the
  // repository over the same `Pool` lets future agent-loop owners persist
  // spec-0018 rows without owning a second DB handle.
  if (options.trace_enabled) {
    impl->trace_repository = std::make_unique<storage::TraceRepository>(*impl->audit_pool);
  }

  return RuntimeAssembly{std::move(impl)};
}

}  // namespace orangutan::bootstrap
