# Permissions And Hooks

The runtime is observable and controllable at well-defined points. **Permissions**
gate effectful actions; **hooks** publish lifecycle events so external code can react.
The legacy code's hook surface was narrow (tool lifecycle + a few message events) and
permissions used compile-time regex (`ctre`) — v2 expands both.

## Permission Engine

> **Engine status (2026-05-17):** the foundation slice ships under
> `oran-permission` — `Verdict { allow, deny, ask }`, `Mode { strict,
> default_, permissive, sandboxed }`, `Rule`, `Decision`, and `RuleSet`
> with the deny → allow → ask precedence below. Tool-name matching is a
> simple `*`-glob; capability-aware gating is wired in via an
> optional `Rule::capability` (of `core::Capability`) and the
> capability-aware `RuleSet::evaluate(tool_name,
> required_capabilities, mode)` overload (the legacy
> `evaluate(tool_name, mode)` is retained as a wrapper that passes an
> empty span). The three-layer "Sources" merge below is now live
> end-to-end: layer-1 is `Defaults::for_mode(Mode)`, layer-2 is the
> parsed `config.permissions` block exposed by `oran-config` as
> `PermissionsConfig`, layer-3 is each agent's
> `agents.<name>.permissions` overlay, and
> `permission::materialize(Mode, global, per_agent) -> RuleSet`
> concatenates the three into a single `RuleSet` that feeds the
> existing deny → allow → ask precedence walk (so an explicit
> `deny` in any layer outranks an `allow` in any other layer).
> Runtime input regex landed on 2026-05-17 as
> `permission::InputPattern` (re2 partial match) — rules now carry
> an optional `Rule::input_pattern`, and the four-argument
> `RuleSet::evaluate(tool_name, input, required_capabilities,
> mode)` overload pays re2's match cost only when a rule is scoped
> to it (see the `bench/permission/scenarios/input_pattern.cpp`
> A-vs-B). Config-side parsing followed in the same day:
> `oran-config` now reads `input_pattern` on each rule, compiles
> the regex once via re2 at load to surface syntactically invalid
> patterns with their JSON path attached (closing
> `0008-permissions.md` criterion 4's "invalid patterns at load
> time are reported" guarantee), and `permission::materialize`
> recompiles the validated source into a runtime `InputPattern`
> when it assembles each `Rule`. `materialize` now returns
> `core::Result<RuleSet>` so a (theoretical) re2 compile failure
> on the validated source surfaces as
> `Error::invalid_argument` rather than silently dropping a
> rule. **HMAC-signed approvals landed on 2026-05-17 too**:
> `permission::ApprovalSecret` wraps libsodium's
> `crypto_auth_hmacsha256` over a 32-byte per-process key
> (`randombytes_buf` source, `sodium_memcmp` constant-time
> compare, `sodium_memzero` on move/destruction); the public
> header is sodium-free (rule C6). On top of it,
> `permission::ApprovalToken` + `permission::ApprovalAuthority`
> own the issue/verify flow described under "Approval Signing"
> in `secrets-and-state.md` — SHA-256 input hash + 16-byte
> random nonce + `core::Time` expiry MACed over a
> domain-separated length-prefixed canonical bytes layout
> (`"oran-approval-v1"` prefix + 1-byte version sentinel +
> length-prefixed tool/identity + input_hash + nonce + LE int64
> millis expiry). `verify` checks expiry → tool → identity →
> input hash → MAC in that order and attaches a `reason`
> context entry (`expired`/`tool_mismatch`/`identity_mismatch`/
> `input_mismatch`/`mac_mismatch`) on the first failure so the
> upcoming audit slice can record *why* a token was rejected.
> `0008-permissions.md` criterion 5 ("Approval signing key is
> rotated when the runtime restarts; prior approvals are
> invalidated") is closed: every process generates a fresh
> 32-byte secret, so a token signed by the previous process
> fails MAC verification under the new authority — the bench
> bucket pins this at ~9.3 µs verify_ok vs. ~36 ns early-reject
> expired (~250× faster). **Replay tracking landed on
> 2026-05-17 too**: `permission::ApprovalBroker` wraps the
> authority with a `(tool, identity, input_hash)`-keyed map of
> `{expires_at, remaining_uses}`; `approve(grant, now)` issues
> a token and registers the entry with
> `remaining_uses = replay_max`; `check(...)` calls
> `authority.verify` first (forwarding its `reason` context
> entries verbatim — `expired`/`tool_mismatch`/
> `identity_mismatch`/`input_mismatch`/`mac_mismatch`), then
> looks up the entry and either decrements or returns
> `reason=no_grant` (entry missing) / `reason=replay_exhausted`
> (counter at zero). Re-approving the same triple overwrites
> the entry; `reap_expired(now)` provides explicit periodic
> eviction. Slice 56 adds the spec-0012 bounded-state ceiling:
> `approve` lazily reaps expired entries, then retains at most
> `ApprovalBroker::max_grants_per_identity` (64) live entries per
> identity. Inserting a new distinct triple beyond the ceiling evicts
> that identity's oldest grant; an evicted token still passes the
> authority's cryptographic checks but broker lookup fails with
> `reason=no_grant`. The per-rule `replay_max` / `approval_ttl_seconds`
> config fields flow through `oran-config` (negative or
> non-integer values reject at load), through `permission::Rule`
> (defaults `replay_max=8`, `approval_ttl=3600s` matching the
> design-doc baseline below), and into `permission::Decision`
> so the agent loop can pass them straight to
> `ApprovalBroker::approve` via an `ApprovalGrant`.
> `0008-permissions.md` criterion 2's replay half is now
> closed. Bench: `broker_approve` ~9.9 µs, `broker_check_ok`
> ~10.7 µs (authority verify + map find + decrement),
> `broker_check_no_grant` / `broker_check_exhausted` ~10.9 µs /
> ~11.0 µs — the broker's overhead over the raw authority
> verify is ~875 ns. **Audit pipeline landed on 2026-05-17 too**:
> the `audit.db` schema + `storage::AuditRepository` ship on the
> storage side, and the permission side now owns
> `permission::AuditEvent`, `permission::AuditOutcome`
> (`allow`/`deny`/`ask`/`approved`/`rejected`), the abstract
> `permission::AuditSink` interface
> (`Awaitable<Result<void>> record(AuditEvent)`), plus three
> concrete sinks: `NullAuditSink` (no-op default),
> `RecordingAuditSink` (in-memory capture for tests), and
> `StorageAuditSink` (column-by-column translation into
> `storage::AuditRepository::append_event`). Free helpers
> `permission::verdict_to_outcome` and
> `permission::make_audit_event_from_decision` keep callsites
> from duplicating `Decision` field copies, and
> `permission::to_hex(span<const std::byte, 32>)` is the single
> hex encoder the storage adapter and any future webhook sink
> will share. Bench: null sink ~260 ns, recording sink ~360 ns,
> storage sink ~18.1 µs end-to-end through SQLite, hex-encode
> ~62 ns. Bootstrap wiring is the final piece of
> `0008-permissions.md` criterion 1; the
> `orangutan --audit-init [<path>]` flag exercises the audit
> pipeline end-to-end (one-shot `asio::io_context` opens a
> `storage::Pool` for the audit DB and runs
> `storage::AuditRepository::migrate()`; idempotent on re-run).
> **Bootstrap assembly landed on 2026-05-17 too**:
> `bootstrap::RuntimeAssembly::build(workspace, executor, options)`
> returns a move-only value type bundling a fresh
> `permission::ApprovalBroker` (per-process key per criterion 5)
> and the active `permission::AuditSink`. When
> `options.audit_enabled=true`, the assembly internally opens a
> `storage::Pool` against the supplied `runtime_executor`, drives
> `AuditRepository::migrate()` on a one-shot `asio::io_context` so
> the build stays synchronous, and installs a `StorageAuditSink`
> referencing the long-lived repository. When `false`, the
> assembly installs a `NullAuditSink` and never touches the audit
> DB. The agent-loop slice owns the assembly for the lifetime of
> the process. Slice 15 packages the audit/sessions migration SQL
> into `oran-storage` itself via C++26 `#embed`
> (`storage::built_in_audit_migrations()` /
> `storage::built_in_session_migrations()`), so `bootstrap::run`
> defaults the assembly to `audit_enabled=true` regardless of
> CWD; the disk override
> (`AuditRepositoryOptions::migrations_directory`) still wins for
> tests that author one-off schemas under a tempdir. Bench:
> assembly_build_with_audit ~202 µs vs. assembly_build_without_audit
> ~400 ns (the audit pipeline costs ~201 µs per process startup,
> dominated by SQLite open + migration + `Pool::open`; the
> `#embed`-backed migration is slightly faster than the previous
> CWD-scan path because no directory iteration is needed).
> `0008-permissions.md` criterion 1 is now closed in-process; the
> per-call "record on decision" plumbing lands with the first
> tool built-ins or the agent loop scaffolding.
> **Approval-broker dispatch wiring landed 2026-05-17 (slice 21)**:
> `tool::Registry::dispatch` now consults the broker when a
> `Verdict::ask` rule fires and the caller supplies
> `(ApprovalBroker*, ApprovalToken*)` on its `DispatchContext`.
> On `broker.check` success the audit row's outcome flips to
> `approved` and the handler runs; on rejection the outcome
> flips to `rejected`, the audit row's `reason` swaps from the
> rule reason to the broker reason
> (`expired`/`tool_mismatch`/`identity_mismatch`/`input_mismatch`/
> `mac_mismatch`/`no_grant`/`replay_exhausted`), and the broker's
> error is forwarded to the caller verbatim. When no broker or
> no token is supplied, the legacy short-circuit applies
> (`outcome=ask`, `permission_denied` with
> `reason=approval_required`) but the error now also carries
> `decision_reason` + `replay_max` + `approval_ttl_seconds`
> copied from the matched rule so the agent loop can hand them
> straight to `ApprovalBroker::approve` without re-running
> `RuleSet::evaluate`. Allow/deny verdicts ignore the broker
> entirely. Bench (`bench-tool/approval.cpp`):
> `dispatch_ask_short_circuit` ~2.4 µs (baseline — same shape as
> `registry.dispatch_allow` plus the new error-context build),
> `dispatch_ask_approved` ~13.4 µs, `dispatch_ask_rejected`
> ~13.6 µs — the broker-attached path costs ~11 µs over the
> short-circuit, ~10 µs of which is the HMAC verify
> (`bench-permission/approval` shows ~9.3 µs for verify_ok), so
> the registry-side overhead on top of the broker work is
> ~875 ns. The remaining `0008-permissions.md` criterion 2
> render-side flow (operator prompt + token capture) lives in
> the upcoming `oran-agent` slice.

### Sources

Rules come from three layers, merged at runtime:

1. **Built-in defaults** (`oran-permission::Defaults`) — safe baseline.
2. **Global config** — `config.permissions`.
3. **Per-agent overlay** — `config.agents.<name>.permissions`.

Later layers override earlier ones; explicit `deny` always wins over `allow`.

Operators can preview every layer combination without running the agent
loop via `orangutan --explain-rules`. The CLI accepts `--mode
<strict|default|permissive|sandboxed>` (baseline selection) and
`--agent <name>` (per-agent overlay selection); both are optional and
the unflagged invocation prints the design-doc default mode with no
overlay applied. The bootstrap layer exposes the same selectors as
`bootstrap::parse_explain_rules_selector` and
`bootstrap::materialize_rules` so tests and future tooling can build a
merged `RuleSet` programmatically.

### Rule Shape

```cpp
struct Rule {
  Verdict     verdict;        // allow | deny | ask
  std::string tool_pattern;   // glob: "file.*", "shell.exec", "memory.write"
  std::optional<InputPattern> input_pattern;  // optional, runtime regex
  std::optional<Capability>   capability;     // gate by capability
  std::optional<std::string>  reason;         // human-readable in approval prompt
};
```

`InputPattern` is a runtime regex (re2). Examples:

```yaml
allow:
  - file.read
  - file.search
  - "shell.exec(/bin/{ls,cat,head,tail,grep,find}:*)"
deny:
  - "shell.exec(rm:*)"
  - "shell.exec(git push *)"
ask:
  - file.write
  - file.edit
  - "shell.exec"
```

### Modes

A profile selects defaults:

| Mode          | Default for tools not matched by any rule |
| ------------- | ----------------------------------------- |
| `auto`        | allow                                     |
| `default`     | allow read-side; ask for write-side       |
| `permissive`  | allow most; deny only the dangerous       |
| `strict`      | deny by default; allow explicit only       |
| `sandboxed`   | read-side only; deny everything else      |

### Evaluation

`permission::Evaluator::evaluate(tool_name, input, capabilities, identity) -> Verdict`

Algorithm:

1. Resolve effective rule set (defaults + global + per-agent).
2. Apply explicit `deny` rules first; if any match, return `deny`.
3. Apply `allow` rules; first match wins, return `allow`.
4. Apply `ask` rules; first match wins, render approval prompt.
5. Default by mode.

The legacy code's "signed approval prompt" pattern continues. The signature scheme:

- Approval prompt is HMAC-signed with a per-process secret.
- Approval replay is allowed within `approval_ttl` (default 1h) for the same
  `(tool_name, input_hash, identity)` triple.
- TTL and replay count are configurable per rule (`replay_max`, default 8).

### Capability-Aware Gating

A tool's `requires` list (see `tool-runtime.md`) is part of the evaluator's input. A
rule may scope to a capability:

```yaml
allow:
  - "*  capability=read_file"
ask:
  - "*  capability=spawn_subprocess"
deny:
  - "*  capability=runtime_loader"
```

This is more expressive than "match by tool name" and survives tool renames.

### Runtime vs. Compile-Time Regex

Legacy used `ctre`. v2 uses **`re2`** (Google's library). Reasons:

- Patterns are now config-driven (compile-time impossible).
- `re2` has linear time guarantees against pathological input.
- Smaller TU footprint than `ctre`.

`docs/rules/libraries.md` codifies this choice.

## Hook Bus

> **Bus status (2026-05-21, slice 31):** the foundation
> ships as `oran-hook`. `hook::Event` enumerates the 41
> lifecycle events listed below; `hook::Mode { advisory,
> blocking }` plus `default_mode(Event)` annotates each
> with the design-doc semantics ("before" events +
> `permission_ask_rendered` default to `blocking`,
> everything else is `advisory`). `hook::Sink` is the
> abstract base; `hook::InProcessSink` is the first
> concrete implementation (a `std::function<async::
> Awaitable<Result<void>>(Event, Payload)>` callback).
> `hook::Bus` exposes `bind(Sink&, events)` /
> `unbind(Sink&)` and one publish method —
> `publish_advisory(Event, Payload) -> Awaitable<
> PublishOutcome>` — that iterates subscribed sinks in
> subscription order, captures each sink's `Result<void>`
> in a `PublishOutcome::SinkResult` row, and never aborts
> the publish on a sink error (advisory contract). The
> `PublishOutcome` lets the caller surface sink failures
> into logs or audit without coupling the publish to a
> single error policy. `hook::Payload` is a `std::variant`
> that today covers `std::monostate` (placeholder for
> events whose typed shape lands with the producing
> subsystem) plus `ToolBeforePayload` and
> `ToolAfterPayload` — typed shapes for the remaining
> events ship with their producers (provider request /
> response payloads when the Anthropic adapter lands,
> memory payloads when `oran-memory` lands, and so on).
> `Registry::dispatch` consumes the bus through the
> optional `DispatchContext::bus` field: when non-null,
> dispatch publishes `tool_before` after the registry
> resolves the tool def and `tool_after` at every exit
> (handler success, permission deny, broker rejection,
> audit error). Hooks are advisory in this slice — sinks
> observe but cannot veto; the blocking-veto path
> (`publish_blocking`, `EventTraits<E>::Decision`) is
> tracked in `exec-plans/tech-debt-tracker.md` and lands
> when the first blocking consumer needs it (the
> operator-prompt sink for `permission_ask_rendered` is
> the most likely first caller). The render-side flow
> that asks the operator (`permission_ask_rendered`) and
> captures the response (`permission_ask_resolved`)
> therefore still lives in the `oran-agent` slice — slice
> 22 ships the bus that those events will publish
> through, not the events themselves. Bench (`bench-hook`
> + `bench-tool`): `publish_no_sinks` ~242 ns vs.
> `publish_one_sink` ~446 ns vs. `publish_three_sinks`
> ~698 ns (~204 ns first-sink dispatch, ~126 ns per
> additional sink); `dispatch_allow_no_hooks` ~2.1 µs
> vs. `dispatch_allow_with_empty_bus` ~2.4 µs vs.
> `dispatch_allow_with_two_sinks` ~3.0 µs (~346 ns "bus
> attached, nothing listens" tax, ~914 ns "bus attached
> with two observers" tax — small relative to the
> ~18 µs StorageAuditSink record).

### Surface

```cpp
// include/oran/hook/bus.hpp — PUBLIC
namespace orangutan::hook {

enum class Event {
  // agent
  agent_start,
  agent_stop,
  iteration_start,
  iteration_end,
  final_response,
  // provider
  provider_request,
  provider_response,
  provider_error,
  provider_fallback,
  // tool
  tool_before,
  tool_dispatched,
  tool_after,
  tool_error,
  // memory
  memory_read_before,
  memory_read_after,
  memory_write_before,
  memory_write_after,
  memory_forget,
  memory_decay,
  // channel
  channel_start,
  channel_stop,
  channel_inbound,
  channel_outbound_pre,
  channel_outbound_post,
  channel_delivery_error,
  // orchestration
  team_created,
  worker_spawned,
  worker_stopped,
  team_message,
  team_broadcast,
  conversation_completed,
  conversation_aborted,
  // automation
  job_scheduled,
  job_started,
  job_finished,
  job_failed,
  // session
  session_start,
  session_end,
  // permission
  permission_ask_rendered,
  permission_ask_resolved,
  permission_denied,
};

class Bus {
 public:
  // Subscription returns a typed handle whose destruction unsubscribes.
  template <Event E>
  [[nodiscard]] Subscription subscribe(Sink&);
  Subscription subscribe(std::initializer_list<Event>, Sink&);

  // Publish; the bus dispatches to all sinks subscribed to the event.
  async::Awaitable<core::Result<DispatchOutcome>> publish(Event, Payload);
};

}  // namespace orangutan::hook
```

### Sink Kinds

`Sink` is an abstract interface; built-in implementations:

| Sink kind     | When to use                                                  |
| ------------- | ------------------------------------------------------------ |
| `ShellSink`   | External script (the legacy default). Sub-process, JSON on stdin. |
| `InProcessSink` | C++ callback — for code that lives inside the binary itself. |
| `LuaSink`     | (stretch) embedded `sol2` / `luajit` runtime. Hot-reloadable. |
| `WasmSink`    | (stretch) wasmtime; sandboxed.                                |
| `WebhookSink` | HTTP POST to a URL via `oran-http::Client`.                  |

Sinks declare `kind()` and a `Capabilities` struct (e.g. "this sink may block"; the
bus respects blocking-vs-fire-and-forget semantics).

### Synchronous vs. Async Hooks

Each event is annotated `blocking` or `advisory`:

- **Blocking** (e.g., `tool_before`, `memory_write_before`, `permission_ask_rendered`):
  the bus awaits all sinks. A sink may return a `Decision` that vetoes / rewrites /
  proceeds.
- **Advisory** (e.g., `tool_after`, `iteration_end`): the bus fires-and-forgets. Sinks
  cannot veto.

This is statically known per event so the type system can enforce it:

```cpp
template <Event E>
struct EventTraits;

template <>
struct EventTraits<Event::tool_before> { using Decision = ToolBeforeDecision; };

template <>
struct EventTraits<Event::tool_after>  { /* no Decision; advisory */ };
```

### Configuration

```jsonc
{
  "hooks": {
    "sinks": [
      { "id": "shell-1", "kind": "shell", "path": "/.orangutan/hooks/pre-tool.sh" },
      { "id": "audit",   "kind": "webhook", "url": "https://audit.local/event" }
    ],
    "bindings": [
      { "sink": "shell-1", "events": ["tool_before", "tool_after"] },
      { "sink": "audit",   "events": ["permission_denied", "tool_error"] }
    ]
  }
}
```

### Audit

Every blocking hook decision is recorded in `audit.db` with `event`, `sink_id`,
`identity`, `decision`, `latency_ms`. The CLI / web admin can replay an audit log.

### Failure Modes

- A blocking sink that times out (`config.hooks.timeout_ms`, default 2000) → the bus
  treats it as `Error::HookTimeout`. The triggering action is *not* executed; the agent
  receives a `tool.error`-style response.
- A blocking sink that crashes (shell exit ≠ 0) → same as timeout.
- An advisory sink failure → logged at WARN; otherwise ignored.

## Per-Agent Wiring

The bootstrap reads `config.hooks.bindings` once and constructs the bus. Each
agent's `Loop` gets a reference to the same bus. Per-agent subscription filtering can
be done with predicate sinks if needed (rare; usually a sink subscribes to all
agents and filters by `identity` in its payload).

## Hook Surface Discoverability

A meta-tool `hook.events` lists all enumerated events, their `blocking|advisory`
annotation, and their payload shape. Agents can query it to know what they can react
to.

## Anti-Patterns

- Sinks that call back into the agent (calling `provider::System::send` from a
  `tool_after` hook). The blocking-vs-advisory contract is unidirectional; sinks
  observe, they don't drive.
- Hooks used to implement features that should be subsystems. If three different
  consumers want a hook that does the same thing, that's a sign for a real
  subsystem (e.g., cost-tracking should be a subsystem, not a hook).
- Hooks that mutate the agent's working memory directly. The hook payload can carry a
  "suggested-mutation", but applying it is the runtime's job.

## See Also

- [`tool-runtime.md`](tool-runtime.md) — tool dispatch ordering with hooks.
- [`agent-platform.md`](agent-platform.md) — six cross-cutting concerns including
  permissions and hooks.
- [`../product-specs/0008-permissions.md`](../product-specs/0008-permissions.md)
  — concrete v1 deliverables.
