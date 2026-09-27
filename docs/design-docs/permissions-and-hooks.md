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
effects. Configured deny rules still apply. Bootstrap's
`materialize_permissions` compiles the selected baseline, global rule values and
agent overlay once before execution. The permission library consumes the owned
`RuleSet` without importing configuration. Workspace settings remain outside rule
compilation. [Runtime composition](bootstrap-runtime.md) owns the adapter API.

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
The host supplies an approval consumer through the blocking
`permission_ask_rendered` hook.

The broker authenticates grants with a process-owned key. A grant binds tool,
identity, input hash, expiry and replay budget; default rule policy is eight
replays within one hour. Expired, mismatched, tampered or exhausted grants are
rejected. Restart invalidates process grants. A hook rewrite triggers a fresh
check of the final operation. Time spent waiting for a path lock counts toward
grant expiry; a cancelled wait does not consume replay budget.

Workspace authorization is independent of a tool-name allow. Resolve the target
before approval and carry the pinned filesystem authority through the effect.
Audit records the final decision and result; raw secrets do not belong there.
Filesystem argument preparation precedes both authority resolution and approval.
Invalid arguments audit a denial with `reason=invalid_tool_input`; they neither
publish an approval request nor consume an existing grant. Validation uses the
final hook input, so a hook may repair an initially malformed request.

## Hook Contracts

A `hook::Sink` is an owned value: an id, an optional `observe` callback, an
optional `decide` callback and a trust flag. `Bus::subscribe` takes ownership;
there is no sink subclassing, unsubscription or publish outcome.

Gates decide before the effect. The bus asks each `decide` callback in
subscription order and the first non-`proceed` decision wins. An error or
exception becomes a `hook_error` veto; exceeding the blocking timeout becomes
`hook_timeout`. The approval prompt is the exception: it waits for the human's
decision until the turn is cancelled. Tool gates may proceed, veto, rewrite the
input or require approval; the memory write gate accepts only proceed or veto.
The decision trace names every consulted gate and travels to the audit row.

Observers run concurrently, cannot change the result and cannot affect the
publisher or sibling sinks. The bus joins them, or abandons one that ignores
cancellation at the advisory deadline; an abandoned observer keeps its sink
alive until it finishes. Untrusted sinks receive redacted inputs, structured
tool output and memory text; trusted local sinks may inspect originals.

- **Blocking**: `tool_before`, `permission_ask_rendered`, `memory_write_before`.

The event vocabulary follows the production publishers: the agent loop owns
provider observations, registry dispatch owns tool observations, memory bindings
own record observations, and approval resolution owns the prompt gate. Each fact
has one event: `tool_after` reports success or failure, and `provider_response`
names the served target, so a fallback is a served profile that differs from the
route's primary profile. `hook::is_gate` defines the blocking set used by
`publish_blocking<E>`; all other events use advisory publication.

```cpp
enum class Event {
  provider_request,
  provider_response,
  provider_error,
  tool_before,
  tool_after,
  memory_read_after,
  memory_write_before,
  memory_write_after,
  memory_forget,
  permission_ask_rendered,
};
```

[tool-runtime](tool-runtime.md) owns dispatch order;
[async-model](async-model.md) owns cancellation and borrowed state.
