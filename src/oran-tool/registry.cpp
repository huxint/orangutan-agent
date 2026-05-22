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
  event.metadata_json = std::move(metadata_json);
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
  hook::ToolAfterPayload payload{
      .tool_name = std::string{name},
      .input_json = std::string{input_json},
      .who = make_hook_identity(ctx),
      .succeeded = result.has_value(),
      .output_text = result.has_value() ? result->text : std::string{},
      .error_kind = result.has_value() ? std::string{} : std::string{core::enum_name(result.error().kind())},
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
      .error_kind = std::string{core::enum_name(error.kind())},
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
  ctx.resolved_path.reset();
  const auto it = entries_.find(name);
  if (it == entries_.end()) {
    co_return std::unexpected(make_lookup_error(name));
  }
  const auto& entry = it->second;

  const auto started_at = core::time::now_utc();

  // Slice 22: publish `tool_before` now that the registry knows the tool
  // exists. Sinks see every call attempt regardless of how the call is
  // subsequently gated; the publish is advisory so its outcome cannot
  // change the dispatch path.
  if (ctx.bus != nullptr) {
    [[maybe_unused]] auto before_outcome =
        co_await ctx.bus->publish_advisory(hook::Event::tool_before,
                                           build_before_payload(name, input_json, ctx, started_at));
  }

  // Compute the dispatch result inside an inner scope so the
  // `tool_after` publish below sees the final result regardless of which
  // branch in the verdict switch fired. `result` is rebound in every
  // branch — the initial placeholder catches the unreachable "fell off
  // the bottom" case so static analysis sees no uninitialised path.
  core::Result<Output> result = std::unexpected(core::Error::internal("dispatch did not produce a result"));
  {
    auto path_resolution = detail::pre_resolve_tool_path(name, input_json, ctx);
    const auto decision = ctx.rules.evaluate(name, input_json, entry.def.required_capabilities, ctx.mode);

    auto event = build_event(name, input_json, decision, ctx, std::move(path_resolution.metadata_json));

    // Slice 21: when the verdict is `ask` AND the caller supplied both an
    // approval broker and a candidate token, consult the broker BEFORE
    // recording so the audit row carries the final outcome (approved or
    // rejected) rather than the pre-broker `ask`. Allow/deny paths are
    // unaffected — the broker is meaningless without an `ask`-fired rule.
    std::optional<core::Error> broker_rejection;
    const bool broker_consulted = !path_resolution.error.has_value() && decision.verdict == permission::Verdict::ask &&
                                  ctx.approval_broker != nullptr && ctx.approval_token != nullptr;
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
      result = std::unexpected(std::move(recorded).error());
    } else if (path_resolution.error.has_value()) {
      result = std::unexpected(std::move(*path_resolution.error).with("tool", std::string{name}));
    } else {
      // Slice 25: publish `tool_dispatched` only when the handler is
      // about to run — i.e. on `allow`, or on `ask` that the broker
      // promoted to `approved`. Sinks subscribed to this event see
      // only the calls whose handlers will actually execute.
      const bool handler_about_to_run =
          decision.verdict == permission::Verdict::allow ||
          (decision.verdict == permission::Verdict::ask && broker_consulted && !broker_rejection.has_value());
      if (handler_about_to_run && ctx.bus != nullptr) {
        [[maybe_unused]] auto dispatched_outcome = co_await ctx.bus->publish_advisory(
            hook::Event::tool_dispatched,
            build_dispatched_payload(name, input_json, ctx, started_at, decision.verdict));
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
            result = co_await entry.handler(input_json, ctx);
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
          result = co_await entry.handler(input_json, ctx);
          break;
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
          build_error_payload(name, input_json, ctx, started_at, finished_at, result.error()));
    }
    [[maybe_unused]] auto after_outcome =
        co_await ctx.bus->publish_advisory(hook::Event::tool_after,
                                           build_after_payload(name, input_json, ctx, started_at, finished_at, result));
  }

  co_return result;
}

}  // namespace orangutan::tool
