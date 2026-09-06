# Permissions And Hooks

The policy core computes a decision from explicit rules, mode, tool, input and
capabilities. It does not prompt, perform IO or execute the tool. Dispatch owns
those effects after the decision.

## Decisions

`RuleSet` is an owned sequence of rule values; dispatch borrows a const span.
`permission::evaluate` is read-only. Matching uses a tool-name glob, optional
capability scope and optional RE2 input expression. For each required capability,
precedence is explicit deny, then allow, then ask; the mode supplies the unmatched
default. Within a verdict, the first matching rule supplies its reason and
approval policy. An unscoped rule applies to the whole tool.

Every required capability must be authorized. Combine capability decisions as
deny, then ask, then allow; a read grant cannot authorize a tool's write effect.
Combined asks use the smallest replay budget and shortest lifetime. A tool with
no declared capabilities still requires an unscoped rule or the mode default.

Strict and sandboxed modes deny unmatched effects. Default mode asks for unmatched
effects and installs read-side allow rules. Permissive mode allows unmatched
effects. Configured deny rules still apply. Materialization combines the selected
baseline, global rules and agent overlay once before execution.

A future parent/child policy must intersect authority rather than merge an allow
list. That boundary is required before agent collaboration is implemented.

## Approvals

An ask decision requires an explicit trusted consumer or previously issued grant.
With no consumer, dispatch returns `permission_denied` with `approval_required`.
The minimal executable has no interactive approval consumer. Embedders can bind
the blocking `permission_ask_rendered` hook.

The broker authenticates grants with a process-owned key. A grant binds tool,
identity, input hash, expiry and replay budget; default rule policy is eight
replays within one hour. Expired, mismatched, tampered or exhausted grants are
rejected. Restart invalidates process grants. A hook rewrite triggers a fresh
check of the final operation.

Workspace authorization is independent of a tool-name allow. Resolve the target
before approval and carry the pinned filesystem authority through the effect.
Audit records the final decision and result; raw secrets do not belong there.

## Hook Contracts

Blocking consumers interpret decisions before the effect. Advisory consumers
observe outcomes and cannot change the result. Timeouts and bounded child
ownership isolate sinks; a timed-out child may still require a lifetime join.
Default sinks receive redacted inputs; trusted local sinks may inspect originals.

- **Blocking**: `tool_before`, `permission_ask_rendered`, `memory_write_before`.

Tool hooks surround registry execution. Provider hooks report request, response,
error and fallback metadata. Memory hooks report accepted recalls/writes/deletes;
the pre-write hook consumes proceed/veto and rejects unsupported decisions.

The enum remains the current bus vocabulary. Names without a producer are not
implemented lifecycle promises; the runtime reduction is removing such reserves.

```cpp
enum class Event {
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
  job_dropped,
  // session boundary
  session_start,
  session_end,
  // permission ask flow
  permission_ask_rendered,
  permission_ask_resolved,
  permission_denied,
};
```

[tool-runtime](tool-runtime.md) owns dispatch order;
[async-model](async-model.md) owns cancellation and borrowed state.
