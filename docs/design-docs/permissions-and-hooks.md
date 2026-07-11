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
> (`Awaitable<Result<void>> record(AuditEvent)` plus slice-67
> `update_metadata(AuditMetadataUpdate)` enrichment), plus three
> concrete sinks: `NullAuditSink` (no-op default),
> `RecordingAuditSink` (in-memory capture for tests), and
> `StorageAuditSink` (column-by-column translation into
> `storage::AuditRepository::append_event` plus same-row metadata
> replacement through `update_event_metadata`). Slice 79 adds
> `AuditEvent::parent_turn_id` and `AuditMetadataUpdate::parent_turn_id`
> as typed `core::TurnId` optionals; `StorageAuditSink` persists the id into
> `audit_events.parent_turn_id`, and metadata updates match it so same-tool
> calls from different turns cannot overwrite each other's usage metadata.
> Free helpers
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
> into `oran-storage` itself via C++26 `#embed`, slice 78 extends
> the audit DB stream to version 2 with the trace table, and slice 79 extends
> it to version 3 with the nullable `audit_events.parent_turn_id` join key
> (`storage::built_in_audit_migrations()` /
> `storage::built_in_session_migrations()` /
> `storage::built_in_trace_migrations()`), so `bootstrap::run`
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
> **Tool audit usage enrichment landed in slice 67**:
> `tool::Registry::dispatch` keeps the record-before-handler invariant, then
> after a successful handler result and output-cap application it writes
> non-empty `tool::Output::usage` under the same row's `metadata_json.usage`
> via `AuditSink::update_metadata`. This update is best-effort metadata
> enrichment; the permission decision row remains the authoritative durable
> record even if enrichment is ignored by a custom sink.
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
> entirely. Slice 94 adds the dispatch-side blocking prompt bridge:
> when a broker and bus are present but no token was supplied,
> `Registry::dispatch` publishes `permission_ask_rendered` with a typed
> payload, treats a `proceed` decision as operator approval by issuing and
> immediately checking a broker token, optionally returns that token through
> `DispatchContext::approval_token_output`, and treats `veto` as
> `outcome=rejected` / `reason=operator_denied`. Slice 95 adds
> `cli::OperatorPromptSink`, the first terminal renderer for that payload:
> it returns `operator_approved:<identity>` on yes/approve/proceed answers
> and `operator_denied:<identity>` on no/deny/reject answers. Slice 96
> confirms the first agent-loop consumer: direct tool calls issued by
> `agent::Loop` refresh `DispatchContext::now` before entering
> `Registry::dispatch`, so prompt `requested_at` and broker expiry are based on
> the real per-call clock even if the reusable context previously held the
> default epoch. Bench
> (`bench-tool/approval.cpp`):
> `dispatch_ask_short_circuit` ~2.4 µs (baseline — same shape as
> `registry.dispatch_allow` plus the new error-context build),
> `dispatch_ask_approved` ~13.4 µs, `dispatch_ask_rejected`
> ~13.6 µs — the broker-attached path costs ~11 µs over the
> short-circuit, ~10 µs of which is the HMAC verify
> (`bench-permission/approval` shows ~9.3 µs for verify_ok), so
> the registry-side overhead on top of the broker work is
> ~875 ns. The remaining approval work is binding the CLI sink into the
> binary's real agent-loop runtime once that handoff exists.

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
overlay applied. When config declares a provider route, those same flags
select the configured-route runner's permission baseline and per-agent
overlay before prompt execution; the no-provider deterministic shell rejects
selector flags unless `--explain-rules` is active. The bootstrap layer exposes
the same selectors as
`bootstrap::parse_explain_rules_selector` and
`bootstrap::materialize_rules` so tests and future tooling can build a
merged `RuleSet` programmatically.

### Rule Shape

```cpp
struct Rule {
  Verdict     verdict;        // allow | deny | ask
  std::string tool_pattern;   // glob: "File*", "ShellExec", "MemoryWrite"
  std::optional<InputPattern> input_pattern;  // optional, runtime regex
  std::optional<Capability>   capability;     // gate by capability
  std::optional<std::string>  reason;         // human-readable in approval prompt
};
```

`InputPattern` is a runtime regex (re2). Examples:

```yaml
allow:
  - FileRead
  - FileSearch
  - "ShellExec(/bin/{ls,cat,head,tail,grep,find}:*)"
deny:
  - "ShellExec(rm:*)"
  - "ShellExec(git push *)"
ask:
  - FileWrite
  - FileEdit
  - "ShellExec"
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

Skill-management tools are capability-gated too: `invoke_skill` runs a loaded
skill (`SkillInvoke`) and `deactivate_skill` clears its active marker
(`SkillDeactivate`, slice 147), so an operator can allow or deny each
independently. Neither carries an explicit `Defaults::for_mode` rule, so both
inherit the per-mode catch-all (`ask` in `default`, `deny` in
`strict`/`sandboxed`, `allow` in `permissive`).

### Runtime vs. Compile-Time Regex

Legacy used `ctre`. v2 uses **`re2`** (Google's library). Reasons:

- Patterns are now config-driven (compile-time impossible).
- `re2` has linear time guarantees against pathological input.
- Smaller TU footprint than `ctre`.

`docs/rules/libraries.md` codifies this choice.

## Hook Bus

> **Bus status (2026-06-07, slice 194):** the foundation
> ships as `oran-hook`. `hook::Event` enumerates the 41
> lifecycle events listed below; `hook::Mode { advisory,
> blocking }` plus `default_mode(Event)` annotates each
> with the design-doc semantics ("before" events +
> `permission_ask_rendered` default to `blocking`,
> everything else is `advisory`). `hook::Sink` is the
> abstract base and exposes `kind()`, which defaults to
> `SinkKind::default_`; `SinkKind::trusted_local` is the
> explicit opt-in for same-process observers that may
> receive raw structured tool output and unredacted sensitive
> mutation inputs. `hook::InProcessSink`
> is the first concrete implementation (a `std::function<
> async::Awaitable<Result<void>>(Event, PayloadPtr)>`
> callback) and can be constructed with either sink kind.
> `hook::Bus` exposes `bind(Sink&, events)` /
> `unbind(Sink&)`, the advisory
> `publish_advisory(Event, Payload) -> Awaitable<
> PublishOutcome>` method, and the constrained blocking
> `publish_blocking<E>(Payload) -> Awaitable<
> Result<HookDecision>>` method. Advisory publishing starts
> every subscribed sink as a sibling child coroutine, builds
> at most one raw shared immutable payload snapshot and one
> default/redacted snapshot per publish, gathers each sink's
> `Result<void>` in subscription-ordered
> `PublishOutcome::SinkResult` rows, and never aborts
> the publish on a sink error (advisory contract). Parent
> cancellation emits child cancellation signals and then drains
> completions so the caller can safely destroy the borrowed sinks
> after `publish_advisory` returns. The
> `PublishOutcome` lets the caller surface sink failures
> into logs or audit without coupling the publish to a
> single error policy. `hook::Payload` is a `std::variant`
> and `hook::PayloadPtr` is `std::shared_ptr<const Payload>`;
> sinks receive the shared pointer so payload lifetime is safe
> across suspension points without cloning the same structured
> bytes once per subscribed sink. `hook::Payload`
> today covers `std::monostate` (placeholder for
> events whose typed shape lands with the producing
> subsystem) plus `ToolBeforePayload`,
> `ToolDispatchedPayload`, `ToolAfterPayload`, `ToolErrorPayload`, slice
> 94's `PermissionAskRenderedPayload`, slice 126's provider lifecycle
> payloads, slice 179's `MemoryWritePayload` / `MemoryForgetPayload`,
> slice 180's `MemoryReadPayload` / `MemoryReadHitPayload`, slice
> 186's `MemoryDecayPayload`, slice 194's `JobLifecyclePayload`
> lifecycle payloads, and slice 215's `JobDroppedPayload` queue
> backpressure payload. Slice 60 adds the `ToolUsage`
> metrics copied from `tool::Output::usage` onto successful
> `ToolAfterPayload`s without making `oran-hook` depend on
> `oran-tool`; slice 65 adds optional
> `ToolAfterPayload::data_json` for raw serialized structured
> output bytes. `publish_advisory` redacts that field for
> every sink whose `kind()` is not `SinkKind::trusted_local`,
> so default sinks receive the existing text + usage view and
> trusted-local sinks receive the raw data. Slice 152 adds the
> same per-sink redaction channel for tool inputs: lifecycle
> payloads that carry `input_json` also carry an optional
> `redacted_input_json`, and both advisory and blocking bus
> publishes substitute that sanitized view for non-trusted
> sinks. `Registry::dispatch` fills the field for `FileWrite`,
> `FileEdit`, and (as of slice 273) `MemoryRemember` with a compact JSON object containing
> `kind=redacted_tool_input`, `tool_name`, the full
> SHA-256 `input_hash`, `input_bytes`, and the redacted string
> byte counts (`content_bytes` or `old_string_bytes` /
> `new_string_bytes`) where applicable. `MemoryRemember` generic
> tool events expose only the hash/size envelope, never
> id/title/body/tags/linked ids. Malformed file-mutation JSON still receives a
> hash-only redacted view; trusted-local sinks receive the original input.
> Slice 179 adds the same trust boundary for long-term memory writes:
> `MemoryWritePayload` carries the raw `record` plus
> `redacted_record` size/count metadata, and the bus clears
> `record.title`, `record.body`, `record.tags`, and
> `record.linked_record_ids` for sinks whose `kind()` is not
> `SinkKind::trusted_local`. The redacted view keeps id, scope,
> kind, shadow state, title/body byte counts, tag count, and
> linked-record count so default sinks can audit routing without
> receiving memory content.
> Slice 180 extends that trust boundary to long-term memory reads:
> `MemoryReadPayload` carries the raw recall `query`, source label
> (`prompt_boundary` or `MemoryRecall`), limit, kind filters, match
> count, timing, hybrid flag, and hit scores plus records. The bus clears
> `query`, hit titles/bodies/tags, and linked ids for default sinks while
> preserving query byte count and record byte/count metadata; trusted-local
> sinks receive the raw query and records.
> Slice 186 adds `MemoryDecayPayload` for successful long-term retention
> passes. It is metadata-only by construction: source label, identity, scope,
> retention inputs, shadowed count, and timing are present, but decayed record
> contents are not, so default and trusted-local sinks receive the same shape.
> Slice 191 reuses that same payload for caller-driven periodic retention ticks
> from `oran-automation`; no new record-content surface is added. Slice 194
> adds `JobLifecyclePayload` for automation start/outcome metadata. It carries
> identity, source, durable job key/type, scope, schedule/start/finish timing,
> success, success counts, and backend failure kind/message, but not job input
> contents or decayed records. Slice 202 reuses that payload for explicit cron
> due execution, using `job_type=cron` and metadata-only handler outcome
> details. Slice 213 reuses it for explicit triggered handler execution, using
> `job_type=triggered`, the stored triggered job agent key, and metadata-only
> handler outcome details. Slice 215 adds `JobDroppedPayload` for bounded
> automation queue backpressure. It carries identity, source, durable job
> key/type, trigger key, drop reason, queue capacity/size, and scheduled/drop
> timing, but no trigger body, queued payload, prompt bytes, or agent input.
> Typed shapes for
> the remaining non-tool
> events ship with their producers (provider request /
> response payloads now live with the agent/provider lifecycle path; memory
> read/write/delete/decay payloads now live with memory producers; channel
> payloads remain planned until their producers wire in, and so on).
> `Registry::dispatch` consumes the bus through the
> optional `DispatchContext::bus` field: when non-null,
> dispatch first publishes blocking `tool_before` through
> `Bus::publish_blocking<Event::tool_before>` before
> workspace resolution and permission evaluation. Veto,
> hook-error, or malformed-rewrite decisions record
> `AuditOutcome::blocked_by_hook`, skip the handler, publish
> advisory failure events, and return
> `Error::permission_denied` with `reason=blocked_by_hook`;
> rewrite decisions substitute the effective input before
> workspace resolution, permission evaluation, broker checks,
> audit, handler execution, and later hook payloads, then
> record `AuditOutcome::rewritten` on allowed calls; and
> `require_approval` promotes otherwise-allow decisions into
> the existing broker path. Every consulted sink decision is
> serialized in `AuditEvent::metadata_json.hook_decisions`
> (subscription order, up to the first non-`proceed`).
> Dispatch then publishes advisory `tool_dispatched` before
> handlers run on allow / ask-approved paths, `tool_error`
> on error exits, and `tool_after` at every exit. Slice 94 also
> consumes blocking `permission_ask_rendered` for direct dispatch: if an
> `ask` decision has a broker and bus but no replay token, dispatch
> publishes the typed approval payload, treats `proceed` as approval by
> issuing/checking a broker token, treats `veto` as
> `operator_denied`, and records `metadata_json.permission_ask_decisions`.
> A bus with no subscribed ask sink still falls through to the legacy
> `approval_required` error. Slice 95 adds the concrete CLI renderer
> (`cli::OperatorPromptSink`) that turns the typed payload into a terminal
> yes/no question and returns the operator decision through this blocking
> path. Slice 96 pins the fake-provider loop consumer by proving
> `agent::Loop` enters direct dispatch with a fresh wall-clock
> `DispatchContext::now` and restores the caller's previous value after the
> call. Slice 92 adds the
> config-driven blocking
> timeout policy: `config::HooksConfig::timeout_ms` defaults
> to 2000, `bootstrap::RuntimeAssembly` owns the process
> `hook::Bus`, and `bootstrap::run` applies the parsed value
> through `RuntimeAssemblyOptions::hook_blocking_timeout`.
> `Bus::publish_blocking` races each sink against that
> per-sink deadline, synthesizes a veto with
> `reason=hook_timeout` on expiry, and records
> `HookDecisionTrace::elapsed` so direct dispatch serializes
> `metadata_json.hook_decisions[].elapsed_ms`. Slice 156 switches
> advisory publishes from sequential awaits to concurrent fan-out
> while preserving subscription-ordered outcome rows; `bench-hook`
> now measures no-op fan-out/gather overhead at roughly
> `publish_no_sinks` ~325 ns, `publish_one_sink` ~1.63 µs, and
> `publish_three_sinks` ~4.07 µs on the local slice machine.
> `bench-tool` remains `dispatch_allow_no_hooks` ~2.1 µs
> vs. `dispatch_allow_with_empty_bus` ~2.4 µs vs.
> `dispatch_allow_with_two_sinks` ~3.0 µs (~346 ns "bus
> attached, nothing listens" tax, ~914 ns "bus attached
> with two observers" tax — small relative to the
> ~18 µs StorageAuditSink record).
>
> Slice 90 opened the spec-0015 v1 blocking surface:
> `<oran/hook/decision.hpp>` ships `HookDecisionKind {
> proceed, veto, rewrite, require_approval }` and
> `HookDecision { kind, reason, optional<string>
> rewritten_input_json, optional<core::Time>
> approval_expires_at, vector<HookDecisionTrace> trace }`;
> `<oran/hook/event_traits.hpp>`
> ships the empty primary `EventTraits<E>` template plus
> explicit specialisations for the v1 whitelist
> (`tool_before`, `permission_ask_rendered`,
> `memory_write_before`) that set
> `Decision = HookDecision`, and the
> `HasBlockingDecision<E>` concept that constrains
> `Bus::publish_blocking<E>`. `hook::Sink` grows a
> virtual `handle_blocking(Event, PayloadPtr) ->
> Awaitable<Result<HookDecision>>` defaulting to
> `proceed` (the coroutine body lives in
> `src/oran-hook/sink.cpp` to keep public-header
> compile cost bounded). `hook::InProcessSink` adds an
> optional `BlockingCallback` installed via
> `set_blocking_handler`; existing constructors do not
> change. `hook::Bus::publish_blocking<E>(Payload) ->
> Awaitable<Result<HookDecision>>` walks subscribed
> sinks in subscription order, awaits each one's
> `handle_blocking(Event, PayloadPtr)`, applies the same
> shared raw/default-redacted payload snapshots the advisory
> path uses, short-circuits at the first non-`proceed`
> decision, and converts sink `core::Result` errors
> and thrown exceptions into a veto with
> `reason="hook_error: <message> [sink=<id>]"`. With no
> sinks subscribed or every sink returning `proceed`,
> the bus yields a default-constructed `HookDecision{}`.
> Slice 91 adds the dispatch consumer and the audit-side
> `permission::AuditOutcome::blocked_by_hook` / `rewritten`
> enumerators. Slice 92 adds `hook::BusOptions` and
> `HookDecisionTrace::elapsed` for blocking timeouts, plus
> bootstrap/config wiring for `hooks.timeout_ms`. Slice 93 adds
> joinable `event_kind=hook_publish` audit rows for traced blocking
> `tool_before` publishes. Slice 94 adds the typed
> `PermissionAskRenderedPayload` and direct-dispatch
> `permission_ask_rendered` broker bridge. Slice 95 adds the first
> user-visible operator-prompt sink in `oran-cli`, and slice 96 proves the
> broker-backed prompt path from inside `agent::Loop`. Slice 126 adds typed
> provider lifecycle payloads and has `agent::Loop` publish advisory
> `provider_request`, `provider_response`, `provider_error`, and
> `provider_fallback` events through the process bus supplied by bootstrap.
> These payloads are metadata-only by construction: they include identity,
> route/profile/model/protocol, counts, retry settings, usage, stop/error, and
> timing fields, but no prompt text, messages, headers, credentials, or raw
> provider bodies. Slice 179 adds the first memory write/delete lifecycle
> producer at the bootstrap callback boundary: `AgentPromptRunner` publishes
> blocking `memory_write_before` for `MemoryRemember` after parsing/scoping the
> record and before mutating the lexical/vector backends; `veto` returns
> `ErrorKind::permission_denied` with `reason=blocked_by_hook`, while
> `rewrite` and `require_approval` are rejected as unsupported for this
> consumer. Successful writes publish advisory `memory_write_after`, and
> successful `MemoryForget` calls publish advisory `memory_forget`. Slice 180
> adds the read-side advisory producer: prompt-boundary long-term recall and
> the `MemoryRecall` tool publish `memory_read_after` after successful lexical
> or hybrid recall, with query/hit content redacted for default sinks. Blocking
> `memory_read_before` remains a declared event without a runtime consumer. Slice
> 186 adds the startup decay producer: configured-route startup publishes
> advisory `memory_decay` after the optional bounded `Fts5Backend::decay(...)`
> pass succeeds, using build-only
> `RuntimeAssemblyOptions::startup_hook_bindings` for observers that must
> subscribe before startup producers run. Slice 191 adds the periodic
> automation producer: `MemoryRetentionService::tick(...)` publishes advisory
> `memory_decay` after a successful due tick records the run and advances
> `last_fired_at`, but only when the caller supplies a `hook::Bus`. Slice 194
> adds advisory `job_started`, `job_finished`, and `job_failed` publishing from
> that same explicit tick owner: start fires before backend decay, failed fires
> after a backend error is recorded as a failed run, and finished fires after a
> successful run row plus `last_fired_at` advancement. Not-due ticks and service
> instances without a bus publish no lifecycle events; advisory sink failures
> remain non-fatal. Slice 202 adds the cron producer:
> `CronService::execute_due(...)` publishes advisory `job_started` before the
> caller handler, `job_failed` after a handler error while leaving stored cron
> state unadvanced, and `job_finished` only after handler success plus durable
> `last_fired_at` advancement. Cron scan-only ticks and services without a bus
> publish no lifecycle events; advisory sink failures remain non-fatal. Slice
> 215 adds the triggered queue producer: `TriggeredQueue::enqueue(...)`
> publishes advisory `job_dropped` for each drop-newest overflow when callers
> supply a hook bus. Slice 217 has `TriggeredQueue::drain_once(...)` publish
> the same advisory event with `reason=agent_lease_conflict` when a caller opts
> into drop-on-conflict handling for a drained descriptor blocked by an active
> triggered-agent lease. Queue enqueues, explicit receives, successful drains,
> and queue instances without a bus publish no drop events; advisory sink
> failures remain non-fatal.
> `test-hook` now reports 38 cases / 313 assertions.

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
  job_dropped,
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
  void bind(Sink& sink, std::span<const Event> events);
  void bind(Sink& sink, std::initializer_list<Event> events);
  std::size_t unbind(Sink& sink);

  // Advisory publish; the bus dispatches to all sinks subscribed to the event.
  async::Awaitable<PublishOutcome> publish_advisory(Event, Payload);

  // Slice-90 blocking publish (spec 0015 v1). Constrained at the call site
  // by `EventTraits<E>::Decision`; only the v1 whitelist
  // (`tool_before`, `permission_ask_rendered`, `memory_write_before`)
  // satisfies `HasBlockingDecision<E>`.
  template <Event E>
    requires HasBlockingDecision<E>
  async::Awaitable<core::Result<HookDecision>> publish_blocking(Payload payload);
};

}  // namespace orangutan::hook
```

### Sink Kinds

`SinkKind` is a coarse trust label on every `Sink`:

```cpp
enum class SinkKind {
  default_,
  trusted_local,
};
```

`Sink::kind()` defaults to `SinkKind::default_`. A sink may return
`SinkKind::trusted_local` only when it is a same-process observer whose
operator intentionally allowed raw tool-result data to stay in process.
`Bus::publish_advisory` and `Bus::publish_blocking` enforce this before
delivery by building shared immutable raw/default-redacted payload snapshots:
they deliver
`output_text`, timing, error fields, and `usage` to every sink, but clears
`ToolAfterPayload::data_json` for all non-trusted-local sinks. The registry
may publish raw serialized `tool::Output::data_json` once; redaction remains a
bus responsibility so each producer does not need to duplicate the policy.
Provider lifecycle payloads do not need per-sink body redaction because their
public shape never contains prompt text, provider request/response bodies,
headers, credentials, or message content. Memory write payloads do carry raw
record text for trusted in-process observers; default sinks receive the same
payload shape with `record.title`, `record.body`, `record.tags`, and
`record.linked_record_ids` cleared and `redacted_record` populated with byte
and count metadata.

Built-in implementations:

| Sink kind     | When to use                                                  |
| ------------- | ------------------------------------------------------------ |
| `ShellSink`   | External script (the legacy default). Sub-process, JSON on stdin. |
| `InProcessSink` | C++ callback — for code that lives inside the binary itself. |
| `LuaSink`     | (stretch) embedded `sol2` / `luajit` runtime. Hot-reloadable. |
| `WasmSink`    | (stretch) wasmtime; sandboxed.                                |
| `WebhookSink` | HTTP POST to a URL via `oran-http::Client`.                  |

Sinks will also declare a `Capabilities` struct (e.g. "this sink may block"; the
bus respects blocking-vs-fire-and-forget semantics) once blocking sinks land.

### Synchronous vs. Async Hooks

Each event is annotated `blocking` or `advisory`:

- **Blocking** (e.g., `tool_before`, `memory_write_before`, `permission_ask_rendered`):
  the bus awaits all sinks. A sink may return a `Decision` that vetoes / rewrites /
  proceeds.
- **Advisory** (e.g., `tool_after`, `iteration_end`): the bus starts subscribed sinks
  as sibling child coroutines, waits for every completion to build
  `PublishOutcome`, and preserves subscription-ordered result rows. Sinks cannot veto.

Provider lifecycle events are advisory in slice 126. `provider_request` observes
the request boundary but cannot rewrite payloads yet; blocking provider rewrites
remain a later spec-0015 extension.

This is statically known per event so the type system can enforce it:

```cpp
// include/oran/hook/event_traits.hpp — generic HookDecision per spec
// 0015 v1. Specialisations are added for every event that wants to
// expose a blocking decision; events without one (e.g. `tool_after`)
// fail to satisfy `HasBlockingDecision` so `publish_blocking<E>` fails
// to compile against them.

template <Event E>
struct EventTraits {};  // primary: no Decision = advisory only

template <>
struct EventTraits<Event::tool_before>             { using Decision = HookDecision; };
template <>
struct EventTraits<Event::permission_ask_rendered> { using Decision = HookDecision; };
template <>
struct EventTraits<Event::memory_write_before>     { using Decision = HookDecision; };

template <Event E>
concept HasBlockingDecision = requires { typename EventTraits<E>::Decision; }
    && std::same_as<typename EventTraits<E>::Decision, HookDecision>;
```

### Configuration

```jsonc
{
  "hooks": {
    "timeout_ms": 2000,
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
  synthesizes a veto with `reason=hook_timeout`. The triggering action is *not*
  executed; direct tool dispatch records `outcome=blocked_by_hook` plus the offending
  sink id and `elapsed_ms`, and the agent receives a `tool.error`-style response.
- A blocking sink that crashes (shell exit ≠ 0) → same as timeout.
- An advisory sink failure → logged at WARN; otherwise ignored.

## Per-Agent Wiring

The bootstrap reads `config.hooks.bindings` once and constructs the bus. Each
agent's `Loop` gets a reference to the same bus. Today `AgentPromptRunner`
threads `RuntimeAssembly::hook_bus()` plus scope/agent identity metadata into
`agent::Loop`, so provider lifecycle events are visible on configured-route
turns even before external sink bindings land. Per-agent subscription filtering
can be done with predicate sinks if needed (rare; usually a sink subscribes to
all agents and filters by `identity` in its payload).

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
