// src/oran-tool/registry.cpp — registry implementation.

#include <oran/tool/registry.hpp>

#include <algorithm>
#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/core/time.hpp>
#include <oran/hook/bus.hpp>
#include <oran/hook/event.hpp>
#include <oran/hook/payload.hpp>
#include <oran/permission/approval.hpp>
#include <oran/permission/approval_broker.hpp>
#include <oran/permission/audit.hpp>
#include <oran/permission/rule_set.hpp>

#include "_impl/audit_metadata.hpp"
#include "_impl/path_resolution.hpp"
#include "_impl/schema_validation.hpp"

namespace orangutan::tool {

namespace {

[[nodiscard]] core::Error make_lookup_error(std::string_view name) {
  return core::Error::not_found("tool is not registered").with("tool", std::string{name});
}

[[nodiscard]] permission::AuditEvent build_event(std::string_view name,
                                                 std::string_view input_json,
                                                 const permission::Decision& decision,
                                                 const DispatchContext& ctx,
                                                 std::string metadata_json) {
  auto event = permission::make_audit_event_from_decision(decision);
  event.scope_key = ctx.scope_key;
  event.agent_key = ctx.agent_key;
  event.tool_name = std::string{name};
  event.identity = ctx.identity;
  event.input_hash = permission::ApprovalAuthority::input_hash(input_json);
  event.parent_turn_id = ctx.parent_turn_id;
  event.metadata_json = std::move(metadata_json);
  return event;
}

[[nodiscard]] std::string input_hash_hex(std::string_view input_json) {
  return permission::to_hex(permission::ApprovalAuthority::input_hash(input_json));
}

[[nodiscard]] permission::AuditMetadataUpdate build_metadata_update(const permission::AuditEvent& event) {
  return permission::AuditMetadataUpdate{
      .scope_key = event.scope_key,
      .agent_key = event.agent_key,
      .tool_name = event.tool_name,
      .identity = event.identity,
      .input_hash = event.input_hash,
      .parent_turn_id = event.parent_turn_id,
      .previous_metadata_json = event.metadata_json,
  };
}

[[nodiscard]] std::string hook_decision_reason(const hook::HookDecision& decision, std::string_view fallback) {
  return decision.reason.empty() ? std::string{fallback} : decision.reason;
}

[[nodiscard]] permission::Decision hook_blocked_decision(std::string reason) {
  return permission::Decision{
      .verdict = permission::Verdict::deny,
      .reason = std::move(reason),
  };
}

[[nodiscard]] core::Error hook_veto_error(std::string_view name, std::string reason) {
  return core::Error::permission_denied("tool blocked by hook")
      .with("tool", std::string{name})
      .with("reason", "blocked_by_hook")
      .with("hook_reason", std::move(reason));
}

[[nodiscard]] core::Error hook_rewrite_error(std::string_view name, std::string reason) {
  return core::Error::permission_denied("tool rewrite rejected by hook")
      .with("tool", std::string{name})
      .with("reason", "blocked_by_hook")
      .with("hook_reason", std::move(reason));
}

[[nodiscard]] permission::Decision
require_approval_decision(permission::Decision decision, const hook::HookDecision& hook_decision, core::Time now) {
  if (decision.verdict != permission::Verdict::allow) {
    return decision;
  }
  decision.verdict = permission::Verdict::ask;
  decision.reason = hook_decision_reason(hook_decision, "hook require_approval");
  if (hook_decision.approval_expires_at.has_value() && *hook_decision.approval_expires_at > now) {
    decision.approval_ttl = std::chrono::duration_cast<std::chrono::seconds>(
        hook_decision.approval_expires_at->to_system_time_point() - now.to_system_time_point());
  }
  return decision;
}

/// Extract the `reason` context entry the approval broker stamps onto every
/// rejection. Returns an empty view when no such entry exists — callers
/// fall back to the rule's decision reason on that path.
[[nodiscard]] std::string_view broker_reason(const core::Error& error) noexcept {
  const auto entries = error.context();
  const auto it = std::ranges::find_if(entries, [](const auto& kv) { return kv.first == "reason"; });
  return it != entries.end() ? std::string_view{it->second} : std::string_view{};
}

[[nodiscard]] bool has_context(const core::Error& error, std::string_view key, std::string_view value) noexcept {
  return std::ranges::any_of(error.context(),
                             [&](const auto& entry) { return entry.first == key && entry.second == value; });
}

[[nodiscard]] std::string error_kind_label(const core::Error& error) {
  if (has_context(error, "reason", "blocked_by_hook")) {
    return "blocked_by_hook";
  }
  return std::string{core::enum_name(error.kind())};
}

[[nodiscard]] hook::Identity make_hook_identity(const DispatchContext& ctx) {
  return hook::Identity{
      .scope_key = ctx.scope_key,
      .agent_key = ctx.agent_key,
      .identity = ctx.identity,
  };
}

[[nodiscard]] hook::ToolBeforePayload build_before_payload(std::string_view name,
                                                           std::string_view input_json,
                                                           const DispatchContext& ctx,
                                                           core::Time started_at) {
  return hook::ToolBeforePayload{
      .tool_name = std::string{name},
      .input_json = std::string{input_json},
      .who = make_hook_identity(ctx),
      .started_at = started_at,
  };
}

[[nodiscard]] hook::ToolAfterPayload build_after_payload(std::string_view name,
                                                         std::string_view input_json,
                                                         const DispatchContext& ctx,
                                                         core::Time started_at,
                                                         core::Time finished_at,
                                                         const core::Result<Output>& result) {
  auto usage = hook::ToolUsage{};
  if (result.has_value()) {
    usage = hook::ToolUsage{
        .bytes_read = result->usage.bytes_read,
        .bytes_written = result->usage.bytes_written,
        .files_touched = result->usage.files_touched,
        .match_count = result->usage.match_count,
        .cost_estimate = result->usage.cost_estimate,
        .wall_time = result->usage.wall_time,
        .truncated = result->usage.truncated,
        .data_dropped = result->usage.data_dropped,
    };
  }
  hook::ToolAfterPayload payload{
      .tool_name = std::string{name},
      .input_json = std::string{input_json},
      .who = make_hook_identity(ctx),
      .succeeded = result.has_value(),
      .output_text = result.has_value() ? result->text : std::string{},
      .data_json = result.has_value() ? result->data_json : std::nullopt,
      .usage = usage,
      .error_kind = result.has_value() ? std::string{} : error_kind_label(result.error()),
      .error_message = result.has_value() ? std::string{} : std::string{result.error().message()},
      .started_at = started_at,
      .finished_at = finished_at,
      .duration = std::chrono::duration_cast<std::chrono::nanoseconds>(finished_at.to_system_time_point() -
                                                                       started_at.to_system_time_point()),
  };
  return payload;
}

[[nodiscard]] hook::ToolDispatchedPayload build_dispatched_payload(std::string_view name,
                                                                   std::string_view input_json,
                                                                   const DispatchContext& ctx,
                                                                   core::Time started_at,
                                                                   permission::Verdict verdict) {
  return hook::ToolDispatchedPayload{
      .tool_name = std::string{name},
      .input_json = std::string{input_json},
      .who = make_hook_identity(ctx),
      .started_at = started_at,
      .verdict = std::string{core::enum_name(verdict)},
  };
}

[[nodiscard]] hook::ToolErrorPayload build_error_payload(std::string_view name,
                                                         std::string_view input_json,
                                                         const DispatchContext& ctx,
                                                         core::Time started_at,
                                                         core::Time finished_at,
                                                         const core::Error& error) {
  return hook::ToolErrorPayload{
      .tool_name = std::string{name},
      .input_json = std::string{input_json},
      .who = make_hook_identity(ctx),
      .error_kind = error_kind_label(error),
      .error_message = std::string{error.message()},
      .started_at = started_at,
      .finished_at = finished_at,
      .duration = std::chrono::duration_cast<std::chrono::nanoseconds>(finished_at.to_system_time_point() -
                                                                       started_at.to_system_time_point()),
  };
}

}  // namespace

core::Result<void> Registry::add(core::ToolDef def, Handler handler) {
  if (def.name.empty()) {
    return std::unexpected(core::Error::invalid_argument("tool definition must have a non-empty name"));
  }
  if (!handler) {
    return std::unexpected(core::Error::invalid_argument("tool handler must not be empty").with("tool", def.name));
  }
  if (auto valid_schema = detail::validate_input_schema(def.name, def.input_schema_json); !valid_schema) {
    return std::unexpected(std::move(valid_schema).error());
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
  const auto it = entries_.find(name);
  if (it == entries_.end()) {
    return std::unexpected(make_lookup_error(name));
  }
  entries_.erase(it);
  return {};
}

const core::ToolDef* Registry::find(std::string_view name) const {
  const auto it = entries_.find(name);
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
  struct RegistryContextGuard {
    DispatchContext& ctx;
    const Registry* previous_registry;

    ~RegistryContextGuard() {
      ctx.registry = previous_registry;
    }
  };

  RegistryContextGuard registry_guard{.ctx = ctx, .previous_registry = ctx.registry};
  ctx.registry = this;
  ctx.resolved_path.reset();
  const auto it = entries_.find(name);
  if (it == entries_.end()) {
    co_return std::unexpected(make_lookup_error(name));
  }
  const auto& entry = it->second;

  const auto started_at = core::time::now_utc();
  std::optional<std::string> rewritten_input;
  std::string_view effective_input = input_json;
  hook::HookDecision hook_decision{};
  bool hook_blocked = false;
  bool hook_rewrote = false;
  bool hook_requires_approval = false;
  std::string hook_reason;
  std::optional<core::Error> blocking_publish_error;

  if (ctx.bus != nullptr) {
    auto before_decision = co_await ctx.bus->publish_blocking<hook::Event::tool_before>(
        build_before_payload(name, input_json, ctx, started_at));
    if (!before_decision) {
      blocking_publish_error = std::move(before_decision).error();
    } else {
      hook_decision = std::move(*before_decision);
      switch (hook_decision.kind) {
        case hook::HookDecisionKind::proceed:
          break;
        case hook::HookDecisionKind::veto:
          hook_blocked = true;
          hook_reason = hook_decision_reason(hook_decision, "hook veto");
          break;
        case hook::HookDecisionKind::rewrite:
          hook_reason = hook_decision_reason(hook_decision, "hook rewrite");
          if (!hook_decision.rewritten_input_json.has_value()) {
            hook_blocked = true;
            hook_reason = "hook rewrite missing rewritten_input_json";
          } else {
            rewritten_input = std::move(*hook_decision.rewritten_input_json);
            effective_input = *rewritten_input;
            hook_rewrote = true;
          }
          break;
        case hook::HookDecisionKind::require_approval:
          hook_requires_approval = true;
          hook_reason = hook_decision_reason(hook_decision, "hook require_approval");
          break;
      }
    }
  }

  // Compute the dispatch result inside an inner scope so the
  // `tool_after` publish below sees the final result regardless of which
  // branch in the verdict switch fired. `result` is rebound in every
  // branch — the initial placeholder catches the unreachable "fell off
  // the bottom" case so static analysis sees no uninitialised path.
  core::Result<Output> result = std::unexpected(core::Error::internal("dispatch did not produce a result"));
  std::optional<permission::AuditMetadataUpdate> audit_metadata_update;
  if (blocking_publish_error.has_value()) {
    result = std::unexpected(std::move(*blocking_publish_error).with("tool", std::string{name}));
  } else if (hook_blocked) {
    const auto metadata_json =
        detail::with_hook_decision_metadata("{}", hook_decision.trace, input_hash_hex(input_json), std::nullopt);
    auto event = build_event(name, effective_input, hook_blocked_decision(hook_reason), ctx, metadata_json);
    event.outcome = permission::AuditOutcome::blocked_by_hook;

    if (auto recorded = co_await ctx.audit.record(std::move(event)); !recorded) {
      result = std::unexpected(std::move(recorded).error());
    } else {
      result =
          std::unexpected(hook_decision.kind == hook::HookDecisionKind::rewrite ? hook_rewrite_error(name, hook_reason)
                                                                                : hook_veto_error(name, hook_reason));
    }
  } else {
    auto path_resolution = detail::pre_resolve_tool_path(name, effective_input, ctx);
    auto decision = ctx.rules.evaluate(name, effective_input, entry.def.required_capabilities, ctx.mode);
    if (hook_requires_approval) {
      decision = require_approval_decision(std::move(decision), hook_decision, ctx.now);
    }

    auto metadata_json = std::move(path_resolution.metadata_json);
    if (hook_rewrote) {
      metadata_json = detail::with_hook_decision_metadata(metadata_json,
                                                          hook_decision.trace,
                                                          input_hash_hex(input_json),
                                                          input_hash_hex(effective_input));
    } else if (!hook_decision.trace.empty()) {
      metadata_json = detail::with_hook_decision_metadata(metadata_json, hook_decision.trace);
    }

    auto event = build_event(name, effective_input, decision, ctx, std::move(metadata_json));
    if (hook_rewrote && decision.verdict == permission::Verdict::allow) {
      event.outcome = permission::AuditOutcome::rewritten;
    }

    // Slice 21: when the verdict is `ask` AND the caller supplied both an
    // approval broker and a candidate token, consult the broker BEFORE
    // recording so the audit row carries the final outcome (approved or
    // rejected) rather than the pre-broker `ask`. Allow/deny paths are
    // unaffected — the broker is meaningless without an `ask`-fired rule.
    std::optional<core::Error> broker_rejection;
    const bool broker_consulted = !path_resolution.error.has_value() && decision.verdict == permission::Verdict::ask &&
                                  ctx.approval_broker != nullptr && ctx.approval_token != nullptr;
    if (broker_consulted) {
      auto checked = ctx.approval_broker->check(*ctx.approval_token, name, effective_input, ctx.identity, ctx.now);
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

    // Keep the decision row durable before the handler runs. If the handler
    // later returns measured usage, the post-result enrichment updates the
    // newest matching row's metadata instead of appending a second audit row.
    const bool handler_about_to_run =
        !path_resolution.error.has_value() &&
        (decision.verdict == permission::Verdict::allow ||
         (decision.verdict == permission::Verdict::ask && broker_consulted && !broker_rejection.has_value()));
    if (handler_about_to_run) {
      audit_metadata_update = build_metadata_update(event);
    }

    if (auto recorded = co_await ctx.audit.record(std::move(event)); !recorded) {
      result = std::unexpected(std::move(recorded).error());
    } else if (path_resolution.error.has_value()) {
      result = std::unexpected(std::move(*path_resolution.error).with("tool", std::string{name}));
    } else {
      // Slice 25: publish `tool_dispatched` only when the handler is
      // about to run — i.e. on `allow`, or on `ask` that the broker
      // promoted to `approved`. Sinks subscribed to this event see
      // only the calls whose handlers will actually execute.
      if (handler_about_to_run && ctx.bus != nullptr) {
        [[maybe_unused]] auto dispatched_outcome = co_await ctx.bus->publish_advisory(
            hook::Event::tool_dispatched,
            build_dispatched_payload(name, effective_input, ctx, started_at, decision.verdict));
      }

      switch (decision.verdict) {
        case permission::Verdict::deny:
          result = std::unexpected(core::Error::permission_denied("tool denied by permission rules")
                                       .with("tool", std::string{name})
                                       .with("reason", decision.reason));
          break;
        case permission::Verdict::ask:
          if (broker_rejection.has_value()) {
            result = std::unexpected(std::move(*broker_rejection).with("tool", std::string{name}));
          } else if (broker_consulted) {
            // Broker accepted — run handler. The audit row already records
            // `approved`.
            result = co_await entry.handler(effective_input, ctx);
          } else {
            result = std::unexpected(core::Error::permission_denied("tool requires approval")
                                         .with("tool", std::string{name})
                                         .with("reason", "approval_required")
                                         .with("decision_reason", decision.reason)
                                         .with("replay_max", std::to_string(decision.replay_max))
                                         .with("approval_ttl_seconds", std::to_string(decision.approval_ttl.count())));
          }
          break;
        case permission::Verdict::allow:
          result = co_await entry.handler(effective_input, ctx);
          break;
      }
    }
  }

  if (result.has_value()) {
    [[maybe_unused]] const auto cap_report = apply_output_caps(*result, ctx.output_caps);
    if (audit_metadata_update.has_value()) {
      auto metadata_json = detail::with_usage_metadata(audit_metadata_update->previous_metadata_json, result->usage);
      if (metadata_json.has_value()) {
        audit_metadata_update->metadata_json = std::move(*metadata_json);
        [[maybe_unused]] auto updated = co_await ctx.audit.update_metadata(std::move(*audit_metadata_update));
      }
    }
  }

  // Slice 22 + 25: publish `tool_error` (failure-only narrow channel) when
  // the dispatch produced an error, then `tool_after` with the dispatch
  // outcome (always). Both share the same `finished_at` so sinks can
  // correlate. Publishes are advisory so their outcome cannot change the
  // returned result.
  if (ctx.bus != nullptr) {
    const auto finished_at = core::time::now_utc();
    if (!result) {
      [[maybe_unused]] auto error_outcome = co_await ctx.bus->publish_advisory(
          hook::Event::tool_error,
          build_error_payload(name, effective_input, ctx, started_at, finished_at, result.error()));
    }
    [[maybe_unused]] auto after_outcome = co_await ctx.bus->publish_advisory(
        hook::Event::tool_after,
        build_after_payload(name, effective_input, ctx, started_at, finished_at, result));
  }

  co_return result;
}

}  // namespace orangutan::tool
