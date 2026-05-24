// src/oran-permission/audit.cpp — permission audit sink implementations.

#include <oran/permission/audit.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <ranges>
#include <span>
#include <string>
#include <utility>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/error.hpp>
#include <oran/permission/rule_set.hpp>

namespace orangutan::permission {

namespace {

constexpr char nibble_to_hex(std::uint8_t nibble) noexcept {
  return static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
}

[[nodiscard]] bool matches_update(const AuditEvent& event, const AuditMetadataUpdate& update) {
  return event.event_kind == update.event_kind && event.scope_key == update.scope_key &&
         event.agent_key == update.agent_key && event.tool_name == update.tool_name &&
         event.identity == update.identity && event.input_hash == update.input_hash &&
         event.parent_turn_id == update.parent_turn_id && event.metadata_json == update.previous_metadata_json;
}

}  // namespace

async::Awaitable<core::Result<void>> AuditSink::update_metadata(AuditMetadataUpdate /*update*/) {
  co_return core::Result<void>{};
}

async::Awaitable<core::Result<void>> NullAuditSink::record(AuditEvent /*event*/) {
  co_return core::Result<void>{};
}

async::Awaitable<core::Result<void>> RecordingAuditSink::record(AuditEvent event) {
  events_.push_back(std::move(event));
  co_return core::Result<void>{};
}

async::Awaitable<core::Result<void>> RecordingAuditSink::update_metadata(AuditMetadataUpdate update) {
  auto reversed = events_ | std::views::reverse;
  auto it = std::ranges::find_if(reversed, [&](const AuditEvent& event) { return matches_update(event, update); });
  if (it == reversed.end()) {
    co_return std::unexpected(
        core::Error::not_found("audit event metadata row was not found").with("tool", std::move(update.tool_name)));
  }
  it->metadata_json = std::move(update.metadata_json);
  co_return core::Result<void>{};
}

AuditEvent make_audit_event_from_decision(const Decision& decision) {
  AuditEvent event;
  event.verdict = decision.verdict;
  event.outcome = verdict_to_outcome(decision.verdict);
  event.reason = decision.reason;
  return event;
}

std::string to_hex(std::span<const std::byte, 32> input_hash) {
  std::string out;
  out.reserve(64);
  for (auto byte : input_hash) {
    const auto value = static_cast<std::uint8_t>(byte);
    out.push_back(nibble_to_hex(static_cast<std::uint8_t>(value >> 4)));
    out.push_back(nibble_to_hex(static_cast<std::uint8_t>(value & 0x0F)));
  }
  return out;
}

}  // namespace orangutan::permission
