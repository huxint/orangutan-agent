// include/oran/bootstrap/runtime_assembly.hpp — per-process runtime services
// assembled at boot.
//
// `RuntimeAssembly` is the long-lived bundle the agent loop inherits from
// `oran-bootstrap`: a fresh `permission::ApprovalBroker` (per-process key
// per `0008-permissions.md` criterion 5) plus the active
// `permission::AuditSink` (storage-backed by default, `NullAuditSink`
// when audit is disabled), the workspace resolver, hook bus, build-only startup
// hook bindings for startup lifecycle producers, optional trace repository,
// optional session-memory store, and optional long-term memory runtime.
//
// The assembly intentionally does *not* own the `async::Runtime`. The
// caller (bootstrap today, the agent loop tomorrow) builds the
// `async::Runtime` first, passes its executor here so the audit `Pool`
// dispatches onto the same executor as the rest of the agent loop, then
// drives `runtime.run()` whenever it is ready. Keeping the `Runtime` out
// of the assembly means a test or a one-shot operator command can
// substitute an `asio::io_context` executor without paying for a full
// thread-pool boot.
//
// The audit migration that runs as part of `build()` uses a *temporary*
// `asio::io_context` driven inline. The long-lived `Pool` is then opened
// against the caller-supplied executor with the schema already in place
// on disk. This mirrors `--audit-init` and keeps `build()` synchronous,
// so callers do not have to be coroutines themselves.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <asio/any_io_executor.hpp>

#include <oran/automation.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>
#include <oran/permission/approval_broker.hpp>
#include <oran/permission/audit.hpp>
#include <oran/tool/workspace.hpp>

namespace orangutan::hook {
class Bus;
class Sink;
enum class Event : std::uint8_t;
}  // namespace orangutan::hook

namespace orangutan::memory::session {
class Store;
}  // namespace orangutan::memory::session

namespace orangutan::memory::longterm {
class Backend;
class HybridRuntime;
class Runtime;
class VectorBackend;
}  // namespace orangutan::memory::longterm

namespace orangutan::storage {
class TraceRepository;
}  // namespace orangutan::storage

namespace orangutan::bootstrap {

struct LongtermMemoryStartupDecayOptions {
  std::string scope_key{};
  core::Time unused_before{core::Time::epoch()};
  double importance_floor{0.0};
  std::size_t limit{0};
  core::Time decay_at{core::Time::epoch()};

  friend bool operator==(const LongtermMemoryStartupDecayOptions&, const LongtermMemoryStartupDecayOptions&) = default;
};

struct RuntimeStartupHookBinding {
  /// Non-owning sink pointer. The caller must keep the sink alive until
  /// `RuntimeAssembly::build` returns; the assembly unbinds startup-only
  /// subscriptions before returning.
  hook::Sink* sink{nullptr};
  /// Events the sink should observe. Empty is a no-op; a null sink is a
  /// configuration error so startup producers never silently drop a requested
  /// subscription.
  std::vector<hook::Event> events{};
};

struct RuntimeAssemblyOptions {
  /// When non-empty, overrides the default audit DB path. The default
  /// is `<workspace>/.orangutan/audit.db`, matching `--audit-init`.
  std::string audit_db_path{};
  /// When `false`, the assembly installs a `NullAuditSink` and never
  /// touches the audit DB. `runtime_executor` is still used when
  /// session memory is enabled; disable both audit and session memory
  /// for tests that only need the broker/workspace path.
  bool audit_enabled{true};
  /// Reader-pool size handed to the audit `Pool`. The audit workload is
  /// dominated by `append_event` (writer) plus occasional `list_events`
  /// (reader), so a small reader pool is enough; the default mirrors
  /// `--audit-init`.
  std::size_t audit_reader_count{1};
  /// Per-slot statement cache size for the audit `Pool`. The migration
  /// runner + `append_event` reuse the same handful of prepared
  /// statements, so the default mirrors `--audit-init`.
  std::size_t audit_statement_cache_capacity{4};
  /// Workspace policy options threaded into the assembly-owned
  /// `tool::Workspace`. Extra read/write roots widen which canonical
  /// roots count as "inside the workspace" for `file.read` / `file.search`
  /// / `directory.list` and `file.write` / `file.edit` / `file.delete`
  /// respectively. The strings are passed through verbatim;
  /// `tool::Workspace::create` canonicalises and validates each root.
  tool::WorkspaceOptions workspace_options{};
  /// Operator-level trace policy from `config.trace().enabled`. When
  /// `true`, the assembly builds a `storage::TraceRepository` on the shared
  /// audit `Pool` so the upcoming agent loop can persist per-turn rows.
  /// Forced off when `audit_enabled` is `false` — the trace surface joins
  /// audit rows, so there is nothing to write into without an audit DB.
  /// Mirrors the default in `config::TraceConfig` so callers that do not
  /// surface trace policy still get the spec-0018 v1 behaviour.
  bool trace_enabled{true};
  /// Optional explicit retention cutoff for trace rows. When set and tracing
  /// is enabled, `build()` purges `trace_turns` rows whose `started_at_ns` is
  /// older than this Unix-nanosecond timestamp before opening the long-lived
  /// trace repository. The assembly does not read a clock; bootstrap derives
  /// this value from `config.trace.retention_days`.
  std::optional<std::int64_t> trace_retention_started_before_ns{};
  /// Per-sink timeout for blocking hook publishes. Parsed from
  /// `config.hooks.timeout_ms` by bootstrap and applied to the
  /// assembly-owned hook bus.
  std::chrono::milliseconds hook_blocking_timeout{2000};
  /// Optional in-process hook bindings installed immediately after the
  /// assembly-owned bus is constructed and before any startup producers run.
  /// They are unbound before `build()` returns, so they are for startup
  /// lifecycle events such as `memory_decay`; regular configured sinks can
  /// still bind to `hook_bus()` after `build()` returns for prompt/tool
  /// lifecycle events.
  std::vector<RuntimeStartupHookBinding> startup_hook_bindings{};
  /// When non-empty, overrides the default sessions DB path. The default
  /// is `<workspace>/.orangutan/sessions.db`, intentionally separate from
  /// `audit.db`.
  std::string sessions_db_path{};
  /// When `false`, the assembly does not open or migrate the sessions DB
  /// and `session_store()` returns `nullptr`. This keeps tests and future
  /// deterministic no-session modes from paying for session storage.
  bool session_memory_enabled{true};
  /// Reader-pool size handed to the sessions `Pool`.
  std::size_t session_reader_count{2};
  /// Per-slot statement cache size for the sessions `Pool`.
  std::size_t session_statement_cache_capacity{8};
  /// When non-empty, overrides the default long-term memory DB path. The
  /// default is `<workspace>/.orangutan/memory.db`, intentionally separate
  /// from audit and session state.
  std::string longterm_memory_db_path{};
  /// When `false`, the assembly does not open or migrate the long-term memory
  /// DB and `longterm_memory_runtime()` returns `nullptr`. Bootstrap disables
  /// this on the built-in no-provider route so fresh CLI runs do not create
  /// persistent memory state.
  bool longterm_memory_enabled{true};
  /// Reader-pool size handed to the long-term memory `Pool`.
  std::size_t longterm_memory_reader_count{2};
  /// Per-slot statement cache size for the long-term memory `Pool`.
  std::size_t longterm_memory_statement_cache_capacity{16};
  /// Optional startup decay pass over the long-term lexical memory DB. Bootstrap
  /// derives this from `memory.longterm.retention` only for configured-route
  /// runs, so the built-in no-provider CLI path still avoids opening memory
  /// state. The pass runs after migration and before the long-lived pool is
  /// exposed; periodic automation remains a separate owner.
  std::optional<LongtermMemoryStartupDecayOptions> longterm_memory_startup_decay{};
  /// Optional automation-owned periodic retention descriptor. RuntimeAssembly
  /// stores it as startup diagnostics and as the future scheduler seed; build()
  /// does not evaluate it, persist it, run a background loop, or publish
  /// periodic `memory_decay`.
  std::optional<automation::MemoryRetentionJob> longterm_memory_retention_job{};
  /// When `true`, the assembly also opens the optional sqlite-vec vector index
  /// over a separate DB and constructs a `memory::longterm::HybridRuntime`.
  /// Requires an xmake build configured with `--vector_memory=y`.
  bool longterm_vector_memory_enabled{false};
  /// When non-empty, overrides the default vector-memory DB path. The default
  /// is `<workspace>/.orangutan/memory-vectors.db`, kept separate from the
  /// dependency-free lexical `memory.db`.
  std::string longterm_vector_memory_db_path{};
  /// Embedding width used by the bootstrap-owned deterministic text embedder
  /// and the sqlite-vec table schema.
  std::size_t longterm_vector_memory_dimensions{64};
  /// Reader-pool size handed to the vector-memory `Pool`.
  std::size_t longterm_vector_memory_reader_count{2};
  /// Per-slot statement cache size for the vector-memory `Pool`.
  std::size_t longterm_vector_memory_statement_cache_capacity{16};
};

/// Per-process runtime infrastructure. Move-only; only
/// constructible via `RuntimeAssembly::build`.
class RuntimeAssembly {
public:
  /// Build the assembly. `workspace` is the workspace root used to
  /// derive the default database paths (same convention as
  /// `--audit-init`). `runtime_executor` is the executor the long-lived
  /// audit, sessions, and long-term memory `Pool`s will dispatch onto; for the
  /// agent loop this is `async::Runtime::executor()`. When `audit_enabled`,
  /// `session_memory_enabled`, and `longterm_memory_enabled` are all `false`
  /// the executor is unused and may be default-constructed.
  ///
  /// Failure modes (the function returns `Error` instead of throwing):
  ///
  ///   * `Error::invalid_argument` — `workspace` is empty.
  ///   * `Error::io` — could not create an owned database directory.
  ///   * `Error::storage` — `Pool::open` or migration failed.
  ///   * `Error::internal` — libsodium failed to initialize (the
  ///     approval broker could not generate a per-process key).
  [[nodiscard]] static core::Result<RuntimeAssembly>
  build(std::string_view workspace, asio::any_io_executor runtime_executor, RuntimeAssemblyOptions options = {});

  RuntimeAssembly(const RuntimeAssembly&) = delete;
  RuntimeAssembly& operator=(const RuntimeAssembly&) = delete;
  RuntimeAssembly(RuntimeAssembly&&) noexcept;
  RuntimeAssembly& operator=(RuntimeAssembly&&) noexcept;
  ~RuntimeAssembly();

  /// Mutable reference to the active audit sink. Either `StorageAuditSink`
  /// (when `options.audit_enabled` was `true`) or `NullAuditSink` (when
  /// it was `false`). The reference is stable for the lifetime of the
  /// assembly; the assembly outlives every borrower.
  [[nodiscard]] permission::AuditSink& audit_sink() noexcept;

  /// Mutable reference to the broker. The broker owns the per-process
  /// `ApprovalAuthority`; restarting the process invalidates every
  /// previously-issued token per `0008-permissions.md` criterion 5.
  [[nodiscard]] permission::ApprovalBroker& approval_broker() noexcept;

  /// `true` iff `options.audit_enabled` was `true` at build time. Useful
  /// for diagnostics (the bootstrap startup banner) and for tests that
  /// branch on whether the storage backend is active.
  [[nodiscard]] bool audit_enabled() const noexcept;

  /// Filesystem path the audit DB lives at. Empty when audit is
  /// disabled; the resolved absolute path otherwise (relative to the
  /// workspace).
  [[nodiscard]] std::string_view audit_path() const noexcept;

  /// Reference to the assembly-owned `tool::Workspace`. The workspace
  /// is constructed from the bootstrap-supplied workspace root plus the
  /// config-derived `RuntimeAssemblyOptions::workspace_options`; future
  /// tool-dispatch sites thread this reference through
  /// `tool::DispatchContext::workspace`. The reference is stable for the
  /// lifetime of the assembly; the assembly outlives every borrower.
  [[nodiscard]] tool::Workspace& workspace() noexcept;
  [[nodiscard]] const tool::Workspace& workspace() const noexcept;

  /// Pointer to the assembly-owned `storage::TraceRepository`. Non-null
  /// iff `audit_enabled()` is `true` and `options.trace_enabled` was
  /// `true` at build time. Future agent-loop owners read this pointer
  /// into `agent::TraceContext::repository` so spec-0018 trace rows land
  /// on the same audit `Pool` as the cause-chain audit rows. The pointer
  /// is stable for the lifetime of the assembly; the assembly outlives
  /// every borrower.
  [[nodiscard]] storage::TraceRepository* trace_repository() noexcept;

  /// `true` iff a `TraceRepository` was constructed at build time. Useful
  /// for diagnostics (the bootstrap startup banner) and for tests that
  /// branch on whether the trace writer is reachable.
  [[nodiscard]] bool trace_enabled() const noexcept;

  /// Reference to the process-owned hook bus. Bootstrap applies the
  /// configured blocking timeout at assembly build time; later slices bind
  /// configured sinks to this bus and thread it into agent/tool contexts.
  [[nodiscard]] hook::Bus& hook_bus() noexcept;
  [[nodiscard]] const hook::Bus& hook_bus() const noexcept;

  /// Pointer to the assembly-owned typed session memory store. Non-null
  /// iff `options.session_memory_enabled` was `true` at build time. The
  /// store wraps a separate `storage::SessionRepository` over
  /// `<workspace>/.orangutan/sessions.db` by default; audit and trace keep
  /// using `audit.db`.
  [[nodiscard]] memory::session::Store* session_store() noexcept;

  /// `true` iff the sessions DB pool/repository/store were constructed at
  /// build time.
  [[nodiscard]] bool session_memory_enabled() const noexcept;

  /// Filesystem path the sessions DB lives at. Empty when session memory
  /// is disabled; the resolved absolute path otherwise.
  [[nodiscard]] std::string_view sessions_path() const noexcept;

  /// Pointer to the assembly-owned long-term memory backend. Non-null iff
  /// `options.longterm_memory_enabled` was `true` at build time. The backend
  /// wraps a separate `storage::Pool` over `<workspace>/.orangutan/memory.db`
  /// by default.
  [[nodiscard]] memory::longterm::Backend* longterm_memory_backend() noexcept;

  /// Pointer to the assembly-owned long-term memory runtime. Non-null iff the
  /// backend exists. Future prompt runners use this at the prompt boundary to
  /// recall section-5 memory before `agent::Loop` starts.
  [[nodiscard]] memory::longterm::Runtime* longterm_memory_runtime() noexcept;

  /// Pointer to the optional assembly-owned long-term vector backend. Non-null
  /// iff `options.longterm_vector_memory_enabled` was `true` and the sqlite-vec
  /// backend was constructed successfully.
  [[nodiscard]] memory::longterm::VectorBackend* longterm_vector_backend() noexcept;

  /// Pointer to the optional hybrid runtime that composes the lexical backend
  /// with the vector backend.
  [[nodiscard]] memory::longterm::HybridRuntime* longterm_hybrid_runtime() noexcept;

  /// `true` iff the long-term memory DB pool/backend/runtime were constructed
  /// at build time.
  [[nodiscard]] bool longterm_memory_enabled() const noexcept;

  /// Count of records shadowed by the optional startup long-term retention
  /// pass. `std::nullopt` means no startup pass was configured or run; `0`
  /// means the pass ran and found no matching records.
  [[nodiscard]] std::optional<std::size_t> longterm_memory_startup_decay_shadowed_count() const noexcept;

  /// Automation-owned periodic retention descriptor supplied at build time.
  /// Present only when bootstrap mapped configured long-term retention into a
  /// future scheduler seed; the assembly does not run it.
  [[nodiscard]] const std::optional<automation::MemoryRetentionJob>& longterm_memory_retention_job() const noexcept;

  /// `true` iff the vector-memory DB pool/backend/hybrid runtime were
  /// constructed at build time.
  [[nodiscard]] bool longterm_vector_memory_enabled() const noexcept;

  /// Filesystem path the long-term memory DB lives at. Empty when long-term
  /// memory is disabled; the resolved absolute path otherwise.
  [[nodiscard]] std::string_view longterm_memory_path() const noexcept;

  /// Filesystem path the long-term vector-memory DB lives at. Empty when
  /// vector memory is disabled; the resolved absolute path otherwise.
  [[nodiscard]] std::string_view longterm_vector_memory_path() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit RuntimeAssembly(std::unique_ptr<Impl> impl) noexcept;
};

}  // namespace orangutan::bootstrap
