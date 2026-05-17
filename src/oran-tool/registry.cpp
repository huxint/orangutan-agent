// src/oran-tool/registry.cpp — registry implementation.

#include <oran/tool/registry.hpp>

#include <algorithm>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/error.hpp>
#include <oran/permission/approval.hpp>
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

  if (auto recorded = co_await ctx.audit.record(std::move(event)); !recorded) {
    co_return std::unexpected(std::move(recorded).error());
  }

  switch (decision.verdict) {
    case permission::Verdict::deny:
      co_return std::unexpected(core::Error::permission_denied("tool denied by permission rules")
                                    .with("tool", std::string{name})
                                    .with("reason", decision.reason));
    case permission::Verdict::ask:
      co_return std::unexpected(core::Error::permission_denied("tool requires approval")
                                    .with("tool", std::string{name})
                                    .with("reason", "approval_required")
                                    .with("decision_reason", decision.reason));
    case permission::Verdict::allow:
      break;
  }

  co_return co_await entry.handler(input_json, ctx);
}

}  // namespace orangutan::tool
