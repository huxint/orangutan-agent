// include/oran/storage/audit_repository.hpp — audit domain repository.
//
// The audit-events table lives in `<workspace>/.orangutan/audit.db` per
// `docs/design-docs/secrets-and-state.md`. This repository owns the
// schema migration and the typed `append` / `list` / `count` operations
// the permission `AuditSink` adapter writes into.
//
// `permission` itself does not depend on `storage` directly — the audit
// surface is split across two libraries so the sink interface
// (`permission::AuditSink`) can live in the permission layer and be
// implemented by callers that may not be backed by SQLite (e.g. a
// fire-and-forget shell hook). The storage-backed adapter lives in
// `permission/audit.hpp` and dispatches to this repository through a
// thin function pointer indirection.
//
// The repository follows the same pool + statement-cache + async lease
// pattern as `SessionRepository` (see `session_repository.hpp`):
//
//   * `migrate()` loads the SQL files from `migrations/audit/` and
//     applies them inside a `WriterLease`. Re-running is an idempotent
//     no-op.
//   * `append_event` runs on a `WriterLease` and stamps the row's
//     `created_at` from the database side (`strftime('%Y-%m-%dT%H:%M:%fZ')`),
//     so two append calls on the same writer always produce
//     monotonic timestamps.
//   * `list_events` runs on a `ReaderLease` and returns rows in
//     descending `id` order (and therefore descending `created_at`).
//
// Concurrency. The repository is move-only and not thread-safe; the
// underlying `Pool` is. Callers serialize through the pool's
// writer-vs-readers lease discipline.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/storage/migrations.hpp>

namespace orangutan::storage {

class Pool;

/// Inputs to `AuditRepository::append_event`. All required fields must
/// be non-empty; the repository rejects rows that would silently
/// degrade the audit log. `input_hash_hex` is optional (allow/deny
/// rows often do not compute it). `metadata_json` defaults to the
/// empty object so callers may simply leave it default-constructed.
struct AppendAuditEventRequest {
  std::string scope_key;
  std::string agent_key;
  std::string tool_name;
  std::string identity;
  /// Wire spelling of `permission::Verdict` (`allow`/`deny`/`ask`).
  /// Stored as text so future enum extensions surface as
  /// `core::Error::storage` on the read side rather than coercing
  /// silently.
  std::string verdict;
  /// Wire spelling of `permission::AuditOutcome`
  /// (`allow`/`deny`/`ask`/`approved`/`rejected`). Carries the
  /// actual end-state — an `ask` row that was later approved by
  /// the operator lands as `outcome=approved`; an `ask` row that
  /// was never resolved (process restarted, timeout) lands as
  /// `outcome=ask`.
  std::string outcome;
  /// Human-readable explanation. Comes from `Decision::reason` for
  /// rule-engine decisions and from `Error::with("reason", ...)`
  /// context entries for broker rejections.
  std::string reason;
  /// 64-char lowercase hex spelling of SHA-256(input). Empty for
  /// callsites that did not compute the hash; the column ends up
  /// SQL `NULL`.
  std::string input_hash_hex{};
  /// Free-form structured metadata. Defaults to `"{}"`. The
  /// repository validates non-empty but does not check JSON shape;
  /// callers (or a future schema-version bump) own that.
  std::string metadata_json{"{}"};
};

struct AuditEventRecord {
  std::int64_t id{};
  std::string scope_key;
  std::string agent_key;
  std::string tool_name;
  std::string identity;
  std::string verdict;
  std::string outcome;
  std::string reason;
  std::optional<std::string> input_hash_hex;
  std::string metadata_json;
  std::string created_at;
};

/// Filters for `AuditRepository::list_events`. Empty string filters mean
/// "do not filter on this column". The repository always orders the
/// result by `id DESC` so the most recent decisions come first.
struct ListAuditEventsOptions {
  /// Required — every audit query is scoped to a `scope_key` so
  /// multi-tenant runtimes (per `secrets-and-state.md` "Identity And
  /// Scope") cannot accidentally leak rows across scopes.
  std::string scope_key;
  /// Optional secondary filters. Empty means "no filter".
  std::string agent_key{};
  std::string tool_name{};
  std::string outcome{};
  std::size_t limit{50};
};

struct AuditRepositoryOptions {
  std::string migrations_directory;
};

class AuditRepository {
public:
  explicit AuditRepository(Pool& pool, AuditRepositoryOptions options = {}) noexcept;

  [[nodiscard]] async::Awaitable<core::Result<MigrationReport>> migrate();

  [[nodiscard]] async::Awaitable<core::Result<AuditEventRecord>> append_event(AppendAuditEventRequest request);

  [[nodiscard]] async::Awaitable<core::Result<std::vector<AuditEventRecord>>>
  list_events(ListAuditEventsOptions options);

  /// Count of rows visible under the same scope. Useful for the
  /// upcoming `--audit-stats` CLI and for tests that need to assert
  /// "exactly one decision was recorded". `scope_key` must be set.
  [[nodiscard]] async::Awaitable<core::Result<std::int64_t>> count_events(std::string scope_key);

private:
  Pool* pool_{};
  AuditRepositoryOptions options_;
};

}  // namespace orangutan::storage
