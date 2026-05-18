// src/oran-tool/registry.cpp — registry implementation.

#include <oran/tool/registry.hpp>

#include <algorithm>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/error.hpp>
#include <oran/permission/approval.hpp>
#include <oran/permission/approval_broker.hpp>
#include <oran/permission/audit.hpp>
#include <oran/permission/rule_set.hpp>

namespace orangutan::tool {

namespace {

[[nodiscard]] core::Error make_lookup_error(std::string_view name) {
  return core::Error::not_found("tool is not registered").with("tool", std::string{name});
}

[[nodiscard]] permission::AuditEvent build_event(std::string_view name,
                                                 std::string_view input_json,
                                                 const permission::Decision& decision,
                                                 const DispatchContext& ctx) {
  auto event = permission::make_audit_event_from_decision(decision);
  event.scope_key = ctx.scope_key;
  event.agent_key = ctx.agent_key;
  event.tool_name = std::string{name};
  event.identity = ctx.identity;
  event.input_hash = permission::ApprovalAuthority::input_hash(input_json);
  return event;
}

/// Extract the `reason` context entry the approval broker stamps onto every
/// rejection. Returns an empty view when no such entry exists — callers
/// fall back to the rule's decision reason on that path.
[[nodiscard]] std::string_view broker_reason(const core::Error& error) noexcept {
  const auto entries = error.context();
  const auto it = std::ranges::find_if(entries, [](const auto& kv) { return kv.first == "reason"; });
  return it != entries.end() ? std::string_view{it->second} : std::string_view{};
}

}  // namespace

core::Result<void> Registry::add(core::ToolDef def, Handler handler) {
  if (def.name.empty()) {
    return std::unexpected(core::Error::invalid_argument("tool definition must have a non-empty name"));
  }
  if (!handler) {
    return std::unexpected(core::Error::invalid_argument("tool handler must not be empty").with("tool", def.name));
  }
  auto [it, inserted] = entries_.try_emplace(def.name);
  if (!inserted) {
    return std::unexpected(core::Error{core::ErrorKind::conflict, "tool is already registered"}.with("tool", def.name));
  }
  it->second.def = std::move(def);
  it->second.handler = std::move(handler);
  it->second.insertion_index = next_index_++;
  return {};
}

core::Result<void> Registry::remove(std::string_view name) {
  const auto it = entries_.find(std::string{name});
  if (it == entries_.end()) {
    return std::unexpected(make_lookup_error(name));
  }
  entries_.erase(it);
  return {};
}

const core::ToolDef* Registry::find(std::string_view name) const {
  const auto it = entries_.find(std::string{name});
  if (it == entries_.end()) {
    return nullptr;
  }
  return &it->second.def;
}

std::vector<core::ToolDef> Registry::catalog() const {
  std::vector<const Entry*> ordered;
  ordered.reserve(entries_.size());
  for (const auto& [_, entry] : entries_) {
    ordered.push_back(&entry);
  }
  std::ranges::sort(ordered, {}, [](const Entry* entry) { return entry->insertion_index; });
  std::vector<core::ToolDef> defs;
  defs.reserve(ordered.size());
  for (const auto* entry : ordered) {
    defs.push_back(entry->def);
  }
  return defs;
}

async::Awaitable<core::Result<Output>>
Registry::dispatch(std::string_view name, std::string_view input_json, DispatchContext& ctx) const {
  const auto it = entries_.find(std::string{name});
  if (it == entries_.end()) {
    co_return std::unexpected(make_lookup_error(name));
  }
  const auto& entry = it->second;

  const auto decision = ctx.rules.evaluate(name, input_json, entry.def.required_capabilities, ctx.mode);

  auto event = build_event(name, input_json, decision, ctx);

  // Slice 21: when the verdict is `ask` AND the caller supplied both an
  // approval broker and a candidate token, consult the broker BEFORE
  // recording so the audit row carries the final outcome (approved or
  // rejected) rather than the pre-broker `ask`. Allow/deny paths are
  // unaffected — the broker is meaningless without an `ask`-fired rule.
  std::optional<core::Error> broker_rejection;
  const bool broker_consulted =
      decision.verdict == permission::Verdict::ask && ctx.approval_broker != nullptr && ctx.approval_token != nullptr;
  if (broker_consulted) {
    auto checked = ctx.approval_broker->check(*ctx.approval_token, name, input_json, ctx.identity, ctx.now);
    if (checked) {
      event.outcome = permission::AuditOutcome::approved;
    } else {
      event.outcome = permission::AuditOutcome::rejected;
      // The audit row's `reason` swaps from the rule reason to the broker
      // reason so a forensic query can tell apart "rule said ask" from
      // "broker said this specific failure". The pre-broker rule reason
      // is still recoverable from `verdict` plus the rule set.
      if (const auto reason = broker_reason(checked.error()); !reason.empty()) {
        event.reason = std::string{reason};
      }
      broker_rejection = std::move(checked).error();
    }
  }

  if (auto recorded = co_await ctx.audit.record(std::move(event)); !recorded) {
    co_return std::unexpected(std::move(recorded).error());
  }

  switch (decision.verdict) {
    case permission::Verdict::deny:
      co_return std::unexpected(core::Error::permission_denied("tool denied by permission rules")
                                    .with("tool", std::string{name})
                                    .with("reason", decision.reason));
    case permission::Verdict::ask:
      if (broker_rejection.has_value()) {
        co_return std::unexpected(std::move(*broker_rejection).with("tool", std::string{name}));
      }
      if (broker_consulted) {
        // Broker accepted — fall through to the handler. The audit row
        // already records `approved`.
        break;
      }
      co_return std::unexpected(core::Error::permission_denied("tool requires approval")
                                    .with("tool", std::string{name})
                                    .with("reason", "approval_required")
                                    .with("decision_reason", decision.reason)
                                    .with("replay_max", std::to_string(decision.replay_max))
                                    .with("approval_ttl_seconds", std::to_string(decision.approval_ttl.count())));
    case permission::Verdict::allow:
      break;
  }

  co_return co_await entry.handler(input_json, ctx);
}

}  // namespace orangutan::tool
