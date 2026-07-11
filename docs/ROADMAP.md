# Roadmap — Subsystem Progress Matrix

> **The handoff surface.** One row per runtime track: where the shipped
> frontier is, what the next slice should be, and what must exist first.
> Read together with [`STATUS.md`](STATUS.md) (the point-in-time snapshot).
> Git records per-change detail; this file aggregates current frontiers.
>
> **Update rule (Prime Directive).** Any slice that moves a track's frontier
> updates that track's row — same commit, no exceptions. See
> [`rules/docs-in-sync.md`](rules/docs-in-sync.md).

## How To Read This File

- **Frontier** — the newest shipped capability on the track, with slice
  numbers (`xmake run orangutan -- --help` reports the current slice).
- **Next step** — the smallest concrete slice the track expects next.
- **Pre-dependencies** — what must land or be decided before that next step
  is buildable. `—` means unblocked. Cross-track blockers are expanded in
  [Dependency Frontier](#dependency-frontier).
- **Refs** — the design doc / product spec / active exec-plan that own the
  track's detail.

## Progress Matrix

| Track | Frontier | Next step | Pre-dependencies | Refs |
| --- | --- | --- | --- | --- |
| **Channels** | Slice 263: `orangutan --serve` owns configured channel ingress with bounded per-channel+conversation dispatch queues, idle worker eviction, structured worker metrics, an operator-facing stderr metrics sink, and a C++ owner/test per-message deadline. Buildable `config.channels[]` entries register into a strand-owned `ChannelManager`, start under the service body, pump adapter `next_message()` results into fan-in, enqueue matching automation triggers when configured, then dispatch each `(channel_id, conversation_id)` key through its own bounded worker so one slow conversation does not block unrelated conversations; an empty worker exits and is erased after its idle TTL. `ServeChannelWorkerMetrics` reports active/max workers plus worker/message counters including deadline timeouts, `ServeChannelMetricsLogSink` emits deduplicated one-line snapshots under `run_serve`, and `ServeChannelOptions::message_deadline` cancels an over-budget agent/reply attempt before sending a still-working fallback reply. The QQ real-smoke gate from slice 238 remains open and `channel_qq` stays default-off. | QQ-port milestone 4b-ii once credentials exist; otherwise channel-local hardening: durable deadline rejoin/later reply or webhook-triggered ingress. For project balance, prefer a non-channel slice next unless a channel blocker emerges. | Real QQ bot credentials and a sendable operator conversation for the QQ smoke run; no hard blocker for generic channel hardening. | [`design-docs/channel-abstraction.md`](design-docs/channel-abstraction.md) · [spec 0003](product-specs/0003-multi-platform-channels.md) · [QQ-port plan](exec-plans/active/2026-06-10-channel-qq-port.md) · [platform APIs](references/messaging-platform-apis.md) |
| **Agent loop (ReAct)** | v1 shipped: seven-section prompt build, provider re-entry tool loop, scheduler-batched parallel dispatch (slice 120), trace rows, provider lifecycle hooks (126), profile-priced cost fill (129). Slice 259 gives the first non-CLI long-lived entry a per-conversation ordering boundary: configured channel messages under `--serve` dispatch through the routed `ChannelPromptRunner` bridge in-order per conversation while unrelated conversations can run concurrently. Slice 270 hosts the CLI and desktop agent loops (with their runner-owned schedulers) on per-run strands, so every production loop honours the scheduler's single-strand contract on multi-worker runtimes. | Harden non-CLI runtime control: split the routed channel agent path onto per-agent strands where the scheduler contract needs finer granularity than the service strand. | — | [spec 0001](product-specs/0001-core-react-loop.md) · [spec 0017](product-specs/0017-fake-provider-first-agent-loop.md) · [`design-docs/agent-platform.md`](design-docs/agent-platform.md) |
| **Prompt framework (sections 1–7, cache)** | Stable: builder + promotion state (70–72), provider cache hints (73), default preamble owner (134), skill section 4 (135–149), memory section 5 (133, 162–167), per-agent overlay (141); stability bench + preamble grep gate in CI. | Changes ride the owning tracks (skills / memory / tools); new surfaces follow [`rules/prompt-design.md`](rules/prompt-design.md) "Adding A New Prompt Surface". | — | [`rules/prompt-design.md`](rules/prompt-design.md) · [spec 0016](product-specs/0016-prompt-and-tool-catalog-cache.md) |
| **Provider portability** | Anthropic Messages + OpenAI Responses, body + SSE streaming end-to-end in the binary (97–124), retry/fallback execution runtime, route/credential resolution, usage rollups (127), slice 245 PascalCase tool names, and slice 247 live configured-route evidence for an Anthropic-compatible streaming prompt plus `DirectoryList` tool round-trip. | Additional protocol families when a configured route needs one, or broader live-provider smoke coverage for an already configured compatible route. | A concrete demand (config naming an unsupported protocol) or secret-safe real-provider test credentials. | [`design-docs/api-portability.md`](design-docs/api-portability.md) |
| **Tools — registry & built-ins** | Seven filesystem/search built-ins + `ToolSearch` + memory/skill built-ins; structured output, usage counters, byte caps (spec 0014 closed for built-ins); shared `parse_input` helpers (115); slice 245 public names are PascalCase (`FileRead`, `MemoryRecall`, `SkillInvoke`, etc.) instead of dotted tool names. Slice 264 extends `DirectoryList` with `recursive=true` for whole-project tree listings under the existing tool name, slice 265 unifies `FileDelete` under the existing tool name so `{path, recursive?}` can delete regular files and explicitly-recursive directories while symlinks remain refused, slice 266 moves recursive filesystem ignore/display decisions into Workspace helpers consumed by `FileSearch` and `DirectoryList`, slice 267 adds the read/list-only per-call `allow_outside_workspace=true` escape with ask approval plus explicit audit display, and slice 273 prevents generic non-trusted tool hooks from receiving raw `MemoryRemember` record fields. | Changes now ride owning tracks; stable unless a tool-runtime owner needs the future capability-gated workspace accessor. | None hard. | [`design-docs/tool-runtime.md`](design-docs/tool-runtime.md) · [spec 0002](product-specs/0002-tool-registry.md) · [spec 0014](product-specs/0014-structured-tool-output.md) |
| **Tool scheduler** | Shipped (116–120): bounded parallelism, per-path read/write locks, per-call timeout, cancellation grace, production loop wiring. Slice 255 closes AC10's background tick: `--serve` shares one strand-hosted `ToolScheduler` across automation jobs and races a `serve_scheduler_reaping` concern that periodically reaps the idle-lock table; `AgentPromptRunner` now optionally borrows an injected `{registry, scheduler}` pair. Slice 259 gives configured-channel `--serve` dispatch a per-conversation worker boundary. Slice 270 hosts the CLI and desktop loops (plus their runner-owned schedulers) on per-run strands with a multi-worker regression test, closing the 2026-06-20 scheduler-executor tech-debt row — every production scheduler now runs on a strand. Slice 273 makes detached cancellation laggards retain scheduler-owned path-lock state until their guards release, so the public scheduler can be destroyed after its grace-window return without a lock-table use-after-free. | v1.1: dispatch singleflight + persisted index caches. Split channel agent runs onto per-agent strands where finer granularity than the service strand is needed. | — | [spec 0012](product-specs/0012-tool-scheduler-and-state.md) |
| **Workspace & path policy** | v1 shipped (55): pre-resolution, confinement, redacted audit metadata. Slice 266 adds `WorkspaceWalkFilter` plus `Workspace::display_path`: recursive `FileSearch` and `DirectoryList` now share hidden-name, built-in low-signal directory, `.gitignore` / `.ignore`, and stable display-label behavior. Slice 267 adds the v1.1 read/list-only per-call outside-workspace override: `allow_outside_workspace=true` can resolve an existing outside target only after promoting the call to ask approval, and audit metadata records the explicit display path. | Stable until `tool::Runtime` lands its future capability-gated `workspace()` accessor. | — | [spec 0013](product-specs/0013-workspace-and-path-policy.md) |
| **File-view & IO caching** | Shipped through 58: range reads, fingerprints, line-offset index, bounded caches, singleflight, inotify watcher. Slice 253 auto-starts the watcher under `orangutan --serve` (the runtime-service owner). | Optional config-driven watch toggle/root; otherwise stable. | — (runtime-service owner shipped, slice 253). | [spec 0011](product-specs/0011-file-view-and-caching.md) · [`design-docs/io-runtime.md`](design-docs/io-runtime.md) |
| **Permissions & approvals** | Rules + materialization, signed approval broker with replay/TTL and per-identity grant caps (56), ask round-trip through the CLI operator sink (94–96). | External hook sink models (config `hooks.sinks` / `hooks.bindings` are accepted placeholders today). | Sink-model design in [`design-docs/permissions-and-hooks.md`](design-docs/permissions-and-hooks.md). | [spec 0008](product-specs/0008-permissions.md) |
| **Hooks & redaction** | Advisory parallel fan-out + blocking publish with timeout (91–92, 156), trust-tiered redaction (65, 152), shared payload snapshots (158), provider/memory lifecycle payloads (126, 179–180). Slice 273 extends generic tool-input redaction to `MemoryRemember`, so default sinks receive only hash/size metadata while trusted-local sinks retain the raw record input. | Blocking `memory_read_before` rewrite/veto consumer (currently deferred). | A consumer design — who rewrites recall queries. | [`design-docs/permissions-and-hooks.md`](design-docs/permissions-and-hooks.md) · [spec 0015](product-specs/0015-blocking-hook-decisions.md) |
| **Memory** | Session store + long-term FTS5 + gated sqlite-vec hybrid (`--vector_memory=y`), recall policy + `MemoryRecall` / `MemoryRemember` / `MemoryForget` tools, read-touch, retention decay + leases (130–196). | Hybrid ranking policy/wiring + embedding ownership (tracker P1–P3); sqlite-vec corpus benchmarks. | Embedding-owner decision (Dependency Frontier #5). | [`design-docs/memory-system.md`](design-docs/memory-system.md) · [spec 0005](product-specs/0005-memory-system.md) |
| **Automation** *(current main line)* | Library-level retention/cron/triggered with durable state, leases, queues, caller-owned finite loops (187–225, plus 183–186 retention prework). Slice 254 gives it a long-lived driver: `orangutan --serve` opens `<workspace>/.orangutan/automation.db`, applies config `automation.cron.jobs[]` seeds once, and drives `AutomationService::run(...)` in a cancel-aware poll loop over a prompt-backed handler (live `HttpProviderBackend`, else offline scripted `FakeProvider`). Slice 257 adds config-authored `automation.triggered.jobs[]` descriptors, bootstrap mapping to `UpsertTriggeredJobRequest`, explicit `AutomationRuntime::apply_triggered_job_seeds(...)`, `RuntimeAssembly::triggered_jobs()` diagnostics storage, and `--serve` seed application when either cron or triggered jobs are configured. Slice 258 wires configured channel ingress into that same service owner: channel messages enqueue matching triggered jobs with trigger key `channel:<channel_id>` before the direct channel reply path runs. Slice 268 adds the non-chat webhook producer seam plus optional triggered payload propagation. Slice 269 binds that seam to `--serve`: `automation.webhooks.listener` can enable a narrow HTTP `POST <path_prefix><id>` listener that feeds `WebhookProducer`, preserves the request body as triggered payload bytes, and lets the existing automation loop drain queued jobs. Slice 273 rejects unauthenticated wildcard/public webhook binds; the first listener is now mechanically loopback-only. | Route concrete automation notifier output back to CLI/channel/desktop, or add authenticated non-loopback webhook intake plus header/deadline/connection bounds. | — (runtime-service owner + automation loop shipped, slice 254; generic channel loop shipped, slice 256; triggered descriptors can be config-authored, slice 257; channel-trigger producer shipped, slice 258; webhook producer seam shipped, slice 268; HTTP listener/config binding shipped, slice 269; loopback-only security boundary enforced, slice 273). | [`design-docs/automation-runtime.md`](design-docs/automation-runtime.md) · [spec 0006](product-specs/0006-automation.md) · [runtime-service-owner plan](exec-plans/completed/2026-06-18-runtime-service-owner.md) · [completed plan](exec-plans/completed/2026-06-07-automation-cron-category.md) |
| **Skills** | Full v1 arc (135–149): catalog renderer, loader + hot reload, `SkillInvoke` / `SkillDeactivate`, activation policy with config deactivation/expiration, durable session activations, event extraction. | Other runtime owners (channels / automation) persist and replay the same activation events. | Those runtime surfaces existing. | [spec 0009](product-specs/0009-skills.md) |
| **CLI** | Streaming render (123), interactive REPL (125), slash commands (128), operator approval sink (95). | Incremental growth as features demand; no open track-local slice. | — | [`design-docs/cli-runtime.md`](design-docs/cli-runtime.md) |
| **Desktop app** | Slices A–D shipped (248–252): prebuilt `slint` package + `--desktop` option + gated shell + `.slint`→C++ codegen + skeleton window (A); `web`→`desktop` config migration — `DesktopConfig{enabled,theme,reduce_motion}` replacing `WebConfig` (B); always-built `oran-desktop` bridge — `ChatViewModel`, `DesktopEventSink`, `ChatBridge` (bounded UI↔runtime `async::Channel` queues + per-turn cancellation), with an injected `provider::EventSink*` on `AgentPromptRunner`; `test-desktop` 15 cases / 59 assertions vs a fake provider (C). Slice 251 lands the always-built Slice-D core — `async::Runtime::start()` (non-blocking launch, runtime coexists with the Slint loop) + `desktop::run_chat_session` (the session loop) with per-turn cancellation and an input-only `close()`; `test-async` 16/83, `test-desktop` 17/70. Slice 252 ships the gated Slint chat tracer — the chat UI (transcript, input, Send/Stop) bound to the `ChatBridge` + the `orangutan --desktop` launch (`run_desktop`: runtime + `RuntimeAssembly` + provider [live `HttpProviderBackend`, else scripted `FakeProvider` fallback] + `AgentPromptRunner` with injected `event_sink`, `Runtime::start()` + `run_chat_session`); the window opens and builds clean (acceptance 1 verified); the operator smoke then closed acceptance 2–3, completing the chat tracer. | Chat tracer complete (slices 248–252; spec 0007 acceptance 1–3); next desktop build-out = post-chat panels (sessions / audit / orchestration DAG) per spec 0007 v1. | None blocking. | [spec 0007](product-specs/0007-web-ui.md) · [`DESKTOP.md`](DESKTOP.md) · [completed plan](exec-plans/completed/2026-06-14-oran-desktop-chat-tracer.md) |
| **Agent teams** | Design/spec only; no code. | First orchestration seam; channel routing shipped (slice 228), so design work can start. | A runtime owner for non-CLI entry helps but does not block the first seam (Dependency Frontier #2). | [`design-docs/team-collaboration.md`](design-docs/team-collaboration.md) · [spec 0004](product-specs/0004-agent-team.md) |
| **Observability & trace** | Slice 244 refines spec-0018 provider cancellation observability on the existing `trace_turns.cancellation_phase` field: parent-cancelled provider awaits now distinguish `provider_initial`, `provider_stream`, and `provider_complete`, and `agent::Loop` returns the same phase in the cancellation error context. | No pre-selected observability slice; broader trace query language or metrics endpoints need a concrete operator/runtime consumer before scoping. | — | [spec 0018](product-specs/0018-first-loop-observability.md) · [`RELIABILITY.md`](RELIABILITY.md) |
| **Build & CI** | `make ci` gates docs/hygiene/docs-sync/status/deps/preamble; supply-chain action pinning; `check-compile-budget.sh` exists but is unwired. | Wire `check-compile-budget.sh` plus xmake build/test into CI. | Reference hardware provisioning (Dependency Frontier #3). | [`BUILD_SYSTEM.md`](BUILD_SYSTEM.md) · [`CICD.md`](CICD.md) · [`rules/compile-budget.md`](rules/compile-budget.md) |

## Dependency Frontier

Cross-track blockers, in rough unblock order. When one of these resolves,
update every row that references it.

1. **Channel ingress chain** — *generic chain resolved 2026-06-10*: mock
   adapter (227) and bootstrap channel routing (228) shipped; the QQ port
   continues in its own plan
   ([`exec-plans/active/2026-06-10-channel-qq-port.md`](exec-plans/active/2026-06-10-channel-qq-port.md)).
   Agent teams and non-CLI agent-loop entry now hang on a runtime owner
   driving receive/dispatch (frontier #2), not on the routing seam.
2. **Runtime-service / daemon owner** — *resolved 2026-06-20*: the owner is
   `orangutan --serve`, a mode of the main binary (not a separate
   `orangutan-server`), tracked by the
   [runtime-service-owner plan](exec-plans/completed/2026-06-18-runtime-service-owner.md).
   Slice A (slice 253) shipped the signal-driven lifecycle plus the **IO watcher
   auto-start** leg; Slice B (slice 254) shipped the **automation service loop** leg
   (opens `automation.db`, applies config cron seeds, drives `AutomationService::run`
   over a live-or-offline-fake provider route); Slice C (slice 255) shipped the
   **scheduler idle-lock reaping tick** — `--serve` shares one strand-hosted
   `ToolScheduler` across automation jobs and reaps its lock table on the same strand
   (the `AgentPromptRunner` registry/scheduler-ownership hoist landed with it). All
   three original legs are shipped. Slice 256 then extends the same owner to configured
   channel receive/dispatch: buildable `channels[]` adapters start under `--serve`,
   pump into `ChannelManager`, dispatch through the routed agent bridge, and stop/drain
   on shutdown. Slice 257 extends the automation leg with config-authored triggered
   descriptors and `--serve` seed application, and slice 258 lets that configured
   channel loop enqueue matching triggered work with `channel:<channel_id>` keys.
   Slice 259 adds the channel dispatch worker boundary above that same fan-in:
   each `(channel_id, conversation_id)` key is serialized independently while
   unrelated conversations can proceed concurrently. Slice 260 bounds that worker
   table by evicting empty conversation workers after their idle TTL, slice
   261 adds structured worker metrics snapshots for the owner/test boundary,
   slice 262 adds a C++ owner/test per-message deadline plus still-working
   fallback reply, and slice 263 binds those metrics snapshots to a
   deduplicated stderr sink under `run_serve`. Webhook/non-chat external
   producers remain open.
   The optional typed `serve` config block is deferred until more than one concern
   wants tuning.
3. **Reference hardware for CI** — gates `check-compile-budget.sh` in
   `ci.sh` plus CI xmake build/test wiring (tech-debt rows 2026-05-20 /
   2026-05-21).
4. **Recursive list / unified delete reshape** — *resolved 2026-06-28*:
   slice 264 ships `DirectoryList recursive=true`, supplying the second
   recursive consumer that unblocks workspace v1.1's shared ignore predicate,
   and slice 265 unifies `FileDelete` for regular files plus explicitly
   recursive directories without adding per-kind delete tools.
5. **Embedding ownership decision** — local deterministic embedding vs. a
   provider-backed one; gates hybrid ranking policy/wiring beyond the
   current `oran-local-text-v1`.
6. **Desktop docs-plan completion** — *resolved 2026-06-10*: the docs
   repivot plan is complete and archived. Slice A (slice 248) then shipped the
   prebuilt `slint` package and the gated `oran-desktop` shell; the remaining
   desktop work (config migration → bridge → chat tracer) is tracked on the
   Desktop row and its active plan, with no cross-track blocker.

## See Also

- [`STATUS.md`](STATUS.md) — point-in-time focus and verified baseline.
- [`product-specs/index.md`](product-specs/index.md) — per-spec shipping
  status.
- [`exec-plans/tech-debt-tracker.md`](exec-plans/tech-debt-tracker.md) —
  open debt rows backing several pre-dependencies above.
