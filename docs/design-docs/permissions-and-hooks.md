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

A child dispatch carries an immutable borrowed `PolicyView` for its parent.
Dispatch evaluates both policies independently against the concrete tool, final
input and every required capability, then applies `permission::intersect`.
Deny dominates ask, which dominates allow. When both decisions ask, the replay
budget and approval lifetime use their respective minima. An allow in one policy
cannot satisfy an unmatched or denied requirement in the other. Scheduler context
snapshots retain this parent policy, so hook rewrites and automatic recall obey
the same restriction.

`AgentRun` itself requires `spawn_agent`. The host assigns a fresh child approval
identity and does not forward parent grants; any child approval binds that child
and the exact final input.

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

The event vocabulary follows the production publishers: the agent loop owns
provider observations, registry dispatch owns tool observations, memory bindings
own record observations, and approval resolution owns the prompt gate.
`EventTraits` is the blocking-admission contract used by `publish_blocking<E>`;
all other events use advisory publication. Payloads carry the corresponding tool,
memory, provider or approval values.

```cpp
enum class Event {
  provider_request,
  provider_response,
  provider_error,
  provider_fallback,
  tool_before,
  tool_dispatched,
  tool_after,
  tool_error,
  memory_read_after,
  memory_write_before,
  memory_write_after,
  memory_forget,
  permission_ask_rendered,
};
```

[tool-runtime](tool-runtime.md) owns dispatch order;
[async-model](async-model.md) owns cancellation and borrowed state.
