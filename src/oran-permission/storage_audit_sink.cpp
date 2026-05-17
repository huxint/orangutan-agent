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

}  // namespace orangutan::permission
