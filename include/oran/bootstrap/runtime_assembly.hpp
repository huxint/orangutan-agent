// include/oran/bootstrap/runtime_assembly.hpp — per-process permission +
// audit infrastructure assembled at boot.
//
// `RuntimeAssembly` is the long-lived bundle the agent loop inherits from
// `oran-bootstrap`: a fresh `permission::ApprovalBroker` (per-process key
// per `0008-permissions.md` criterion 5) plus the active
// `permission::AuditSink` (storage-backed by default, `NullAuditSink`
// when audit is disabled). The bundle is the wiring point the
// `Next intended slice` bullet in `docs/STATUS.md` calls out — "Wire a
// per-process `ApprovalBroker` + `StorageAuditSink` into runtime
// assembly so the upcoming agent loop inherits both".
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

#include <memory>
#include <string>
#include <string_view>

#include <asio/any_io_executor.hpp>

#include <oran/core/result.hpp>
#include <oran/permission/approval_broker.hpp>
#include <oran/permission/audit.hpp>
#include <oran/tool/workspace.hpp>

namespace orangutan::storage {
class TraceRepository;
}  // namespace orangutan::storage

namespace orangutan::bootstrap {

struct RuntimeAssemblyOptions {
  /// When non-empty, overrides the default audit DB path. The default
  /// is `<workspace>/.orangutan/audit.db`, matching `--audit-init`.
  std::string audit_db_path{};
  /// When `false`, the assembly installs a `NullAuditSink` and never
  /// touches the audit DB. `runtime_executor` is ignored on this path,
  /// so callers without an `async::Runtime` (e.g. tests for the
  /// approval broker in isolation) can build the assembly cheaply.
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
};

/// Per-process permission + audit infrastructure. Move-only; only
/// constructible via `RuntimeAssembly::build`.
class RuntimeAssembly {
public:
  /// Build the assembly. `workspace` is the workspace root used to
  /// derive the default audit-DB path (same convention as
  /// `--audit-init`). `runtime_executor` is the executor the long-lived
  /// audit `Pool` will dispatch onto; for the agent loop this is
  /// `async::Runtime::executor()`. When `options.audit_enabled` is
  /// `false` the executor is unused and may be default-constructed.
  ///
  /// Failure modes (the function returns `Error` instead of throwing):
  ///
  ///   * `Error::invalid_argument` — `workspace` is empty.
  ///   * `Error::io` — could not create the audit directory.
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

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit RuntimeAssembly(std::unique_ptr<Impl> impl) noexcept;
};

}  // namespace orangutan::bootstrap
