// include/oran/permission/storage_audit_sink.hpp — SQLite-backed audit sink.
//
// Adapts the permission-side `AuditSink` interface
// (`include/oran/permission/audit.hpp`) onto the storage-side
// `storage::AuditRepository` (`include/oran/storage/audit_repository.hpp`).
// Lives in `oran-permission` rather than in `oran-storage` so the
// permission library owns the entire audit surface; the storage
// library stays just a typed CRUD wrapper over `audit_events`.
//
// The adapter is the natural composition point because:
//
//   * The agent loop already speaks `permission::AuditSink` —
//     declaring the adapter here lets it construct a
//     `StorageAuditSink` next to its other permission objects.
//   * Switching backends is one constructor argument away
//     (`StorageAuditSink` vs. `RecordingAuditSink` vs. a future
//     `WebhookAuditSink`).
//   * `oran-bootstrap` can build the adapter and hand the agent
//     loop a typed sink reference without dragging
//     `storage::AuditRepository` into the agent runtime layer.
//
// `oran-permission` already depends on `oran-storage` transitively
// through `oran-config` (see `xmake/targets.lua`), so this header can
// freely include `<oran/storage/audit_repository.hpp>` without
// crossing a library-boundary line.

#pragma once

#include <oran/permission/audit.hpp>
#include <oran/storage/audit_repository.hpp>

namespace orangutan::storage {
class AuditRepository;
}

namespace orangutan::permission {

/// Audit sink that writes every recorded event into a
/// `storage::AuditRepository`. The repository's `append_event` is the
/// only call this adapter makes; mapping is column-for-column —
/// `AuditEvent::input_hash` becomes 64-char hex via `permission::to_hex`,
/// and the enums (`Verdict`, `AuditOutcome`) flow through their
/// generic `core::enum_name` spelling.
///
/// The adapter does not own the repository; the caller (typically
/// `oran-bootstrap`) keeps the `AuditRepository` alive for at least
/// the lifetime of every sink built on top of it.
class StorageAuditSink final : public AuditSink {
public:
  explicit StorageAuditSink(storage::AuditRepository& repository) noexcept;

  [[nodiscard]] async::Awaitable<core::Result<void>> record(AuditEvent event) override;

private:
  storage::AuditRepository* repository_{};
};

}  // namespace orangutan::permission
