// include/oran/hook/event.hpp — enumerated hook events.
//
// Slice 22 introduces `oran-hook` as the lifecycle observability surface for
// the runtime. Every cross-cutting integration point that wants to react to a
// runtime decision (audit, render an operator prompt, dispatch a shell sink,
// publish a webhook) does it by subscribing to one of these events on the
// `hook::Bus`.
//
// The enum lists every event the design contemplates so subscribers, sinks,
// and the meta-tool `hook.events` can iterate the universe without library
// bumps. The events without typed payloads in `payload.hpp` carry
// `std::monostate` for now — the typed shape lands with the producing
// subsystem (provider lifecycle payloads now ship with the agent/provider path;
// memory read/write/delete/decay payloads ship with their producers, and so on).

#pragma once

#include <cstdint>

namespace orangutan::hook {

/// Every runtime lifecycle event the bus knows about. Wire spelling and
/// parse come from `core::enum_name` / `core::parse_enum` — no per-enum
/// forwarding shim (see `docs/rules/code-style.md` "Enums").
enum class Event : std::uint8_t {
  // agent lifecycle
  agent_start,
  agent_stop,
  iteration_start,
  iteration_end,
  final_response,
  // provider lifecycle
  provider_request,
  provider_response,
  provider_error,
  provider_fallback,
  // tool dispatch lifecycle
  tool_before,
  tool_dispatched,
  tool_after,
  tool_error,
  // memory tier events
  memory_read_before,
  memory_read_after,
  memory_write_before,
  memory_write_after,
  memory_forget,
  memory_decay,
  // channel adapters
  channel_start,
  channel_stop,
  channel_inbound,
  channel_outbound_pre,
  channel_outbound_post,
  channel_delivery_error,
  // orchestration / teams
  team_created,
  worker_spawned,
  worker_stopped,
  team_message,
  team_broadcast,
  conversation_completed,
  conversation_aborted,
  // automation jobs
  job_scheduled,
  job_started,
  job_finished,
  job_failed,
  // session boundary
  session_start,
  session_end,
  // permission ask flow
  permission_ask_rendered,
  permission_ask_resolved,
  permission_denied,
};

/// Per-event semantics. `advisory` sinks observe and may report errors but
/// cannot block the runtime action; `blocking` sinks are awaited and may
/// veto the action. `Bus` exposes both publish modes; runtime producers
/// choose which path they consume. `tool_before` is the first blocking
/// consumer inside `tool::Registry::dispatch`.
enum class Mode : std::uint8_t {
  advisory,
  blocking,
};

/// Design-doc default mode for `event`. The bus reads this when no
/// per-subscription override is supplied. Most events are advisory; the
/// "before" pre-action gates and the "ask" prompt render are blocking.
[[nodiscard]] constexpr Mode default_mode(Event event) noexcept {
  switch (event) {
    case Event::tool_before:
    case Event::memory_write_before:
    case Event::memory_read_before:
    case Event::permission_ask_rendered:
      return Mode::blocking;
    default:
      return Mode::advisory;
  }
}

}  // namespace orangutan::hook
