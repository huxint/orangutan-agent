// src/oran-bootstrap/runtime_assembly.cpp — per-process runtime service assembly.

#include <oran/bootstrap/runtime_assembly.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/any_io_executor.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <oran/async.hpp>
#include <oran/core/error.hpp>
#include <oran/hook.hpp>
#include <oran/memory.hpp>
#include <oran/permission.hpp>
#include <oran/storage.hpp>
#include <oran/tool/workspace.hpp>

namespace orangutan::bootstrap {
namespace {

using ::orangutan::core::Error;
using ::orangutan::core::Result;

constexpr std::string_view kAuditDatabaseRelative = ".orangutan/audit.db";
constexpr std::string_view kSessionsDatabaseRelative = ".orangutan/sessions.db";
constexpr std::string_view kMemoryDatabaseRelative = ".orangutan/memory.db";
constexpr std::string_view kVectorMemoryDatabaseRelative = ".orangutan/memory-vectors.db";

struct StartupDecayResult {
  std::size_t shadowed_count{0};
  core::Time started_at{};
  core::Time finished_at{};
  std::chrono::nanoseconds duration{0};
};

[[nodiscard]] std::string
resolve_database_path(std::string_view workspace, std::string_view override_path, std::string_view relative_path) {
  if (!override_path.empty()) {
    return std::string{override_path};
  }
  auto path = std::filesystem::path{std::string{workspace}};
  path /= relative_path;
  return path.string();
}

[[nodiscard]] Result<void> ensure_parent_directory(const std::filesystem::path& target, std::string_view kind) {
  if (auto parent = target.parent_path(); !parent.empty()) {
    auto ec = std::error_code{};
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      return std::unexpected(Error::io("failed to create runtime database directory")
                                 .with("path", parent.string())
                                 .with("database", std::string{kind})
                                 .with("detail", ec.message()));
    }
  }
  return {};
}

/// Drive the audit repository migration to completion on a one-shot
/// `asio::io_context`. Mirrors `bootstrap::run_audit_init`'s pattern so the
/// runtime assembly and the `--audit-init` operator command remain
/// behaviourally identical at the schema-migration boundary.
[[nodiscard]] Result<storage::MigrationReport> run_audit_migration_inline(const std::string& audit_path,
                                                                          std::size_t reader_count,
                                                                          std::size_t statement_cache_capacity) {
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

/// Run trace retention against the migrated audit DB before the long-lived pool
/// starts serving runtime trace writes.
[[nodiscard]] Result<std::int64_t> run_trace_retention_inline(const std::string& audit_path,
                                                              std::size_t reader_count,
                                                              std::size_t statement_cache_capacity,
                                                              std::int64_t started_before_ns) {
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
  storage::TraceRepository repo{temp_pool};

  auto deleted = std::int64_t{0};
  auto purge_error = std::optional<Error>{};
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        auto purged = co_await repo.purge_turns_started_before(started_before_ns);
        if (!purged) {
          purge_error = std::move(purged).error();
          co_return;
        }
        deleted = *purged;
        co_return;
      },
      asio::detached);
  io.run();

  if (purge_error) {
    return std::unexpected(std::move(*purge_error));
  }
  return deleted;
}

/// Drive the session repository migration to completion before the long-lived
/// sessions pool is opened on the caller-supplied executor.
[[nodiscard]] Result<storage::MigrationReport> run_session_migration_inline(const std::string& sessions_path,
                                                                            std::size_t reader_count,
                                                                            std::size_t statement_cache_capacity) {
  asio::io_context io;
  auto temp_pool_result = storage::Pool::open(io.get_executor(),
                                              storage::PoolOptions{
                                                  .path = sessions_path,
                                                  .reader_count = reader_count,
                                                  .statement_cache_capacity = statement_cache_capacity,
                                              });
  if (!temp_pool_result) {
    return std::unexpected(std::move(temp_pool_result).error());
  }
  auto temp_pool = std::move(*temp_pool_result);
  storage::SessionRepository repo{temp_pool};

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

/// Drive the long-term memory migration to completion before the long-lived
/// memory pool is opened on the caller-supplied executor.
[[nodiscard]] Result<storage::MigrationReport>
run_longterm_memory_migration_inline(const std::string& memory_path,
                                     std::size_t reader_count,
                                     std::size_t statement_cache_capacity) {
  asio::io_context io;
  auto temp_pool_result = storage::Pool::open(io.get_executor(),
                                              storage::PoolOptions{
                                                  .path = memory_path,
                                                  .reader_count = reader_count,
                                                  .statement_cache_capacity = statement_cache_capacity,
                                              });
  if (!temp_pool_result) {
    return std::unexpected(std::move(temp_pool_result).error());
  }
  auto temp_pool = std::move(*temp_pool_result);
  memory::longterm::Fts5Backend backend{temp_pool};

  auto report = storage::MigrationReport{};
  auto migrate_error = std::optional<Error>{};
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        auto migrated = co_await backend.migrate();
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

/// Run one startup decay pass after long-term memory migration and before the
/// long-lived pool starts serving prompt/tool reads.
[[nodiscard]] std::chrono::nanoseconds duration_between(core::Time started_at, core::Time finished_at) {
  return finished_at.to_system_time_point() - started_at.to_system_time_point();
}

[[nodiscard]] hook::MemoryDecayPayload
make_startup_memory_decay_payload(const LongtermMemoryStartupDecayOptions& options, const StartupDecayResult& result) {
  return hook::MemoryDecayPayload{
      .who =
          hook::Identity{
              .scope_key = options.scope_key,
              .agent_key = "bootstrap",
              .identity = "startup",
          },
      .source = "startup",
      .scope_key = options.scope_key,
      .unused_before = options.unused_before,
      .importance_floor = options.importance_floor,
      .limit = options.limit,
      .decay_at = options.decay_at,
      .shadowed_count = result.shadowed_count,
      .started_at = result.started_at,
      .finished_at = result.finished_at,
      .duration = result.duration,
  };
}

[[nodiscard]] Result<void> bind_startup_hooks(hook::Bus& bus, std::span<const RuntimeStartupHookBinding> bindings) {
  for (const auto& binding : bindings) {
    if (binding.sink == nullptr) {
      return std::unexpected(Error::invalid_argument("runtime hook binding sink is null").with("reason", "null_sink"));
    }
    if (!binding.events.empty()) {
      bus.bind(*binding.sink, std::span<const hook::Event>{binding.events});
    }
  }
  return {};
}

void unbind_startup_hooks(hook::Bus& bus, std::span<const RuntimeStartupHookBinding> bindings) noexcept {
  for (const auto& binding : bindings) {
    if (binding.sink != nullptr) {
      static_cast<void>(bus.unbind(*binding.sink));
    }
  }
}

[[nodiscard]] hook::PublishOutcome publish_startup_memory_decay_inline(hook::Bus& bus,
                                                                       const LongtermMemoryStartupDecayOptions& options,
                                                                       const StartupDecayResult& result) {
  asio::io_context io;
  auto outcome = hook::PublishOutcome{};
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        outcome = co_await bus.publish_advisory(hook::Event::memory_decay,
                                                make_startup_memory_decay_payload(options, result));
        co_return;
      },
      asio::detached);
  io.run();
  return outcome;
}

[[nodiscard]] Result<StartupDecayResult>
run_longterm_memory_startup_decay_inline(const std::string& memory_path,
                                         std::size_t reader_count,
                                         std::size_t statement_cache_capacity,
                                         const LongtermMemoryStartupDecayOptions& options) {
  asio::io_context io;
  auto temp_pool_result = storage::Pool::open(io.get_executor(),
                                              storage::PoolOptions{
                                                  .path = memory_path,
                                                  .reader_count = reader_count,
                                                  .statement_cache_capacity = statement_cache_capacity,
                                              });
  if (!temp_pool_result) {
    return std::unexpected(std::move(temp_pool_result).error());
  }
  auto temp_pool = std::move(*temp_pool_result);
  memory::longterm::Fts5Backend backend{temp_pool};

  auto result = StartupDecayResult{};
  auto decay_error = std::optional<Error>{};
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        result.started_at = core::time::now_utc();
        auto decayed = co_await backend.decay(memory::longterm::DecayRequest{
            .scope_key = options.scope_key,
            .unused_before = options.unused_before,
            .importance_floor = options.importance_floor,
            .limit = options.limit,
            .decay_at = options.decay_at,
        });
        if (!decayed) {
          decay_error = std::move(decayed).error();
          co_return;
        }
        result.finished_at = core::time::now_utc();
        result.duration = duration_between(result.started_at, result.finished_at);
        result.shadowed_count = decayed->shadowed_records.size();
        co_return;
      },
      asio::detached);
  io.run();

  if (decay_error) {
    return std::unexpected(std::move(*decay_error));
  }
  return result;
}

/// Drive the optional vector-memory migration to completion before the
/// long-lived vector pool is opened on the caller-supplied executor.
[[nodiscard]] Result<void> run_longterm_vector_memory_migration_inline(const std::string& vector_path,
                                                                       std::size_t reader_count,
                                                                       std::size_t statement_cache_capacity,
                                                                       std::size_t dimensions) {
  asio::io_context io;
  auto extensions = memory::longterm::SqliteVecBackend::auto_extensions();
  auto temp_pool_result = storage::Pool::open(io.get_executor(),
                                              storage::PoolOptions{
                                                  .path = vector_path,
                                                  .reader_count = reader_count,
                                                  .statement_cache_capacity = statement_cache_capacity,
                                              },
                                              extensions);
  if (!temp_pool_result) {
    return std::unexpected(std::move(temp_pool_result).error());
  }
  auto temp_pool = std::move(*temp_pool_result);
  memory::longterm::SqliteVecBackend backend{temp_pool,
                                             memory::longterm::SqliteVecBackendOptions{
                                                 .dimensions = dimensions,
                                             }};

  auto migrate_error = std::optional<Error>{};
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        auto migrated = co_await backend.migrate();
        if (!migrated) {
          migrate_error = std::move(migrated).error();
          co_return;
        }
        co_return;
      },
      asio::detached);
  io.run();

  if (migrate_error) {
    return std::unexpected(std::move(*migrate_error));
  }
  return {};
}

}  // namespace

struct RuntimeAssembly::Impl {
  bool audit_enabled{false};
  std::string audit_path;
  std::string sessions_path;
  std::string longterm_memory_path;
  std::string longterm_vector_memory_path;
  std::optional<std::size_t> longterm_memory_startup_decay_shadowed_count{};
  std::optional<automation::MemoryRetentionJob> longterm_memory_retention_job{};
  // The members below are non-default-constructible in their final
  // shape and capture pointers into each other (`AuditRepository`
  // refers to `audit_pool`, `audit_sink` refers to `audit_repository`).
  // Heap-allocating each via `unique_ptr` keeps their addresses stable
  // across `RuntimeAssembly` moves: the assembly's `impl_` pointer
  // changes hands, but the Impl itself stays put on the heap.
  std::unique_ptr<storage::Pool> audit_pool;
  std::unique_ptr<storage::AuditRepository> audit_repository;
  std::unique_ptr<storage::TraceRepository> trace_repository;
  std::unique_ptr<storage::Pool> sessions_pool;
  std::unique_ptr<storage::SessionRepository> session_repository;
  std::unique_ptr<memory::session::Store> session_store;
  std::unique_ptr<storage::Pool> longterm_memory_pool;
  std::unique_ptr<memory::longterm::Fts5Backend> longterm_memory_backend;
  std::unique_ptr<memory::longterm::Runtime> longterm_memory_runtime;
  std::unique_ptr<storage::Pool> longterm_vector_memory_pool;
  std::unique_ptr<memory::longterm::SqliteVecBackend> longterm_vector_backend;
  std::unique_ptr<memory::longterm::HybridRuntime> longterm_hybrid_runtime;
  std::unique_ptr<permission::AuditSink> audit_sink;
  std::unique_ptr<permission::ApprovalBroker> approval_broker;
  std::unique_ptr<tool::Workspace> workspace;
  std::unique_ptr<hook::Bus> hook_bus;
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

hook::Bus& RuntimeAssembly::hook_bus() noexcept {
  return *impl_->hook_bus;
}

const hook::Bus& RuntimeAssembly::hook_bus() const noexcept {
  return *impl_->hook_bus;
}

memory::session::Store* RuntimeAssembly::session_store() noexcept {
  return impl_->session_store.get();
}

bool RuntimeAssembly::session_memory_enabled() const noexcept {
  return impl_->session_store != nullptr;
}

std::string_view RuntimeAssembly::sessions_path() const noexcept {
  return impl_->sessions_path;
}

memory::longterm::Backend* RuntimeAssembly::longterm_memory_backend() noexcept {
  return impl_->longterm_memory_backend.get();
}

memory::longterm::Runtime* RuntimeAssembly::longterm_memory_runtime() noexcept {
  return impl_->longterm_memory_runtime.get();
}

memory::longterm::VectorBackend* RuntimeAssembly::longterm_vector_backend() noexcept {
  return impl_->longterm_vector_backend.get();
}

memory::longterm::HybridRuntime* RuntimeAssembly::longterm_hybrid_runtime() noexcept {
  return impl_->longterm_hybrid_runtime.get();
}

bool RuntimeAssembly::longterm_memory_enabled() const noexcept {
  return impl_->longterm_memory_runtime != nullptr;
}

std::optional<std::size_t> RuntimeAssembly::longterm_memory_startup_decay_shadowed_count() const noexcept {
  return impl_->longterm_memory_startup_decay_shadowed_count;
}

const std::optional<automation::MemoryRetentionJob>& RuntimeAssembly::longterm_memory_retention_job() const noexcept {
  return impl_->longterm_memory_retention_job;
}

bool RuntimeAssembly::longterm_vector_memory_enabled() const noexcept {
  return impl_->longterm_hybrid_runtime != nullptr;
}

std::string_view RuntimeAssembly::longterm_memory_path() const noexcept {
  return impl_->longterm_memory_path;
}

std::string_view RuntimeAssembly::longterm_vector_memory_path() const noexcept {
  return impl_->longterm_vector_memory_path;
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
  impl->hook_bus = std::make_unique<hook::Bus>(hook::BusOptions{
      .blocking_timeout = options.hook_blocking_timeout,
  });
  if (auto bound = bind_startup_hooks(*impl->hook_bus,
                                      std::span<const RuntimeStartupHookBinding>{options.startup_hook_bindings});
      !bound) {
    return std::unexpected(std::move(bound).error());
  }

  if (options.session_memory_enabled) {
    impl->sessions_path = resolve_database_path(workspace, options.sessions_db_path, kSessionsDatabaseRelative);
    if (auto parent_ok = ensure_parent_directory(std::filesystem::path{impl->sessions_path}, "sessions"); !parent_ok) {
      return std::unexpected(std::move(parent_ok).error());
    }

    auto session_migration = run_session_migration_inline(impl->sessions_path,
                                                          options.session_reader_count,
                                                          options.session_statement_cache_capacity);
    if (!session_migration) {
      return std::unexpected(std::move(session_migration).error());
    }

    auto sessions_pool = storage::Pool::open(runtime_executor,
                                             storage::PoolOptions{
                                                 .path = impl->sessions_path,
                                                 .reader_count = options.session_reader_count,
                                                 .statement_cache_capacity = options.session_statement_cache_capacity,
                                             });
    if (!sessions_pool) {
      return std::unexpected(std::move(sessions_pool).error());
    }
    impl->sessions_pool = std::make_unique<storage::Pool>(std::move(*sessions_pool));
    impl->session_repository = std::make_unique<storage::SessionRepository>(*impl->sessions_pool);
    impl->session_store = std::make_unique<memory::session::Store>(*impl->session_repository);
  }

  if (!options.longterm_memory_enabled &&
      (options.longterm_memory_startup_decay.has_value() || options.longterm_memory_retention_job.has_value())) {
    return std::unexpected(Error::invalid_argument("long-term retention requires long-term memory")
                               .with("reason", "longterm_memory_disabled"));
  }

  if (options.longterm_memory_enabled) {
    impl->longterm_memory_retention_job = std::move(options.longterm_memory_retention_job);
    impl->longterm_memory_path =
        resolve_database_path(workspace, options.longterm_memory_db_path, kMemoryDatabaseRelative);
    if (auto parent_ok = ensure_parent_directory(std::filesystem::path{impl->longterm_memory_path}, "memory");
        !parent_ok) {
      return std::unexpected(std::move(parent_ok).error());
    }

    auto longterm_migration = run_longterm_memory_migration_inline(impl->longterm_memory_path,
                                                                   options.longterm_memory_reader_count,
                                                                   options.longterm_memory_statement_cache_capacity);
    if (!longterm_migration) {
      return std::unexpected(std::move(longterm_migration).error());
    }

    if (options.longterm_memory_startup_decay.has_value()) {
      auto decayed = run_longterm_memory_startup_decay_inline(impl->longterm_memory_path,
                                                              options.longterm_memory_reader_count,
                                                              options.longterm_memory_statement_cache_capacity,
                                                              *options.longterm_memory_startup_decay);
      if (!decayed) {
        return std::unexpected(std::move(decayed).error());
      }
      impl->longterm_memory_startup_decay_shadowed_count = decayed->shadowed_count;
      [[maybe_unused]] auto outcome =
          publish_startup_memory_decay_inline(*impl->hook_bus, *options.longterm_memory_startup_decay, *decayed);
    }

    auto memory_pool =
        storage::Pool::open(runtime_executor,
                            storage::PoolOptions{
                                .path = impl->longterm_memory_path,
                                .reader_count = options.longterm_memory_reader_count,
                                .statement_cache_capacity = options.longterm_memory_statement_cache_capacity,
                            });
    if (!memory_pool) {
      return std::unexpected(std::move(memory_pool).error());
    }
    impl->longterm_memory_pool = std::make_unique<storage::Pool>(std::move(*memory_pool));
    impl->longterm_memory_backend = std::make_unique<memory::longterm::Fts5Backend>(*impl->longterm_memory_pool);
    impl->longterm_memory_runtime = std::make_unique<memory::longterm::Runtime>(*impl->longterm_memory_backend);
  }

  if (options.longterm_vector_memory_enabled) {
    if (impl->longterm_memory_backend == nullptr) {
      return std::unexpected(Error::invalid_argument("long-term vector memory requires long-term lexical memory")
                                 .with("reason", "longterm_memory_disabled"));
    }
    impl->longterm_vector_memory_path =
        resolve_database_path(workspace, options.longterm_vector_memory_db_path, kVectorMemoryDatabaseRelative);
    if (auto parent_ok =
            ensure_parent_directory(std::filesystem::path{impl->longterm_vector_memory_path}, "vector-memory");
        !parent_ok) {
      return std::unexpected(std::move(parent_ok).error());
    }

    auto vector_migration =
        run_longterm_vector_memory_migration_inline(impl->longterm_vector_memory_path,
                                                    options.longterm_vector_memory_reader_count,
                                                    options.longterm_vector_memory_statement_cache_capacity,
                                                    options.longterm_vector_memory_dimensions);
    if (!vector_migration) {
      return std::unexpected(std::move(vector_migration).error());
    }

    auto extensions = memory::longterm::SqliteVecBackend::auto_extensions();
    auto vector_pool =
        storage::Pool::open(runtime_executor,
                            storage::PoolOptions{
                                .path = impl->longterm_vector_memory_path,
                                .reader_count = options.longterm_vector_memory_reader_count,
                                .statement_cache_capacity = options.longterm_vector_memory_statement_cache_capacity,
                            },
                            extensions);
    if (!vector_pool) {
      return std::unexpected(std::move(vector_pool).error());
    }
    impl->longterm_vector_memory_pool = std::make_unique<storage::Pool>(std::move(*vector_pool));
    impl->longterm_vector_backend = std::make_unique<memory::longterm::SqliteVecBackend>(
        *impl->longterm_vector_memory_pool,
        memory::longterm::SqliteVecBackendOptions{
            .dimensions = options.longterm_vector_memory_dimensions,
        });
    impl->longterm_hybrid_runtime = std::make_unique<memory::longterm::HybridRuntime>(*impl->longterm_memory_backend,
                                                                                      *impl->longterm_vector_backend);
  }

  if (!options.audit_enabled) {
    impl->audit_sink = std::make_unique<permission::NullAuditSink>();
    unbind_startup_hooks(*impl->hook_bus, std::span<const RuntimeStartupHookBinding>{options.startup_hook_bindings});
    return RuntimeAssembly{std::move(impl)};
  }

  impl->audit_path = resolve_database_path(workspace, options.audit_db_path, kAuditDatabaseRelative);
  if (auto parent_ok = ensure_parent_directory(std::filesystem::path{impl->audit_path}, "audit"); !parent_ok) {
    return std::unexpected(std::move(parent_ok).error());
  }

  auto migration =
      run_audit_migration_inline(impl->audit_path, options.audit_reader_count, options.audit_statement_cache_capacity);
  if (!migration) {
    return std::unexpected(std::move(migration).error());
  }
  if (options.trace_enabled && options.trace_retention_started_before_ns.has_value()) {
    auto retained = run_trace_retention_inline(impl->audit_path,
                                               options.audit_reader_count,
                                               options.audit_statement_cache_capacity,
                                               *options.trace_retention_started_before_ns);
    if (!retained) {
      return std::unexpected(std::move(retained).error());
    }
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

  unbind_startup_hooks(*impl->hook_bus, std::span<const RuntimeStartupHookBinding>{options.startup_hook_bindings});
  return RuntimeAssembly{std::move(impl)};
}

}  // namespace orangutan::bootstrap
