// src/oran-permission/storage_audit_sink.cpp — SQLite-backed audit sink.

#include <oran/permission/storage_audit_sink.hpp>

#include <expected>
#include <string>
#include <utility>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/core/result.hpp>
#include <oran/permission/audit.hpp>
#include <oran/storage/audit_repository.hpp>

namespace orangutan::permission {

StorageAuditSink::StorageAuditSink(storage::AuditRepository& repository) noexcept : repository_{&repository} {}

async::Awaitable<core::Result<void>> StorageAuditSink::record(AuditEvent event) {
  storage::AppendAuditEventRequest request{
      .scope_key = std::move(event.scope_key),
      .agent_key = std::move(event.agent_key),
      .tool_name = std::move(event.tool_name),
      .identity = std::move(event.identity),
      .verdict = std::string{core::enum_name(event.verdict)},
      .outcome = std::string{core::enum_name(event.outcome)},
      .reason = std::move(event.reason),
      .parent_turn_id = event.parent_turn_id,
      .metadata_json = std::move(event.metadata_json),
  };
  if (event.input_hash.has_value()) {
    request.input_hash_hex = to_hex(*event.input_hash);
  }

  auto appended = co_await repository_->append_event(std::move(request));
  if (!appended) {
    co_return std::unexpected(std::move(appended).error());
  }
  co_return core::Result<void>{};
}

async::Awaitable<core::Result<void>> StorageAuditSink::update_metadata(AuditMetadataUpdate update) {
  storage::UpdateAuditEventMetadataRequest request{
      .scope_key = std::move(update.scope_key),
      .agent_key = std::move(update.agent_key),
      .tool_name = std::move(update.tool_name),
      .identity = std::move(update.identity),
      .parent_turn_id = update.parent_turn_id,
      .previous_metadata_json = std::move(update.previous_metadata_json),
      .metadata_json = std::move(update.metadata_json),
  };
  if (update.input_hash.has_value()) {
    request.input_hash_hex = to_hex(*update.input_hash);
  }

  auto updated = co_await repository_->update_event_metadata(std::move(request));
  if (!updated) {
    co_return std::unexpected(std::move(updated).error());
  }
  co_return core::Result<void>{};
}

}  // namespace orangutan::permission
