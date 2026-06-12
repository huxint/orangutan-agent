# Roadmap — Subsystem Progress Matrix

> **The handoff surface.** One row per runtime track: where the shipped
> frontier is, what the next slice should be, and what must exist first.
> Read together with [`STATUS.md`](STATUS.md) (the point-in-time snapshot).
> Per-slice detail is canonical under [`histories/`](histories/); this file
> only aggregates frontiers and never restates slice narratives.
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
| **Channels** *(current main line)* | Slice 229: QQ-port milestone 1 — gated `oran-channel-qq` (`--channel_qq=y`, default off) with `qq::TokenStore` (single-flight app-access-token refresh) and `qq::ApiClient` (authenticated requests over `oran-http::Client` with the 401/429/gateway retry ladder and `normalize_api_response`), validated against a scripted loopback HTTP server; on top of the slice-226–228 trait/manager/dispatch/config-routing chain. | QQ-port milestone 2: WebSocket-gateway receive transport (Hello/Identify/heartbeat/Resume) with reconnect backoff behind `Channel::next_message()` (QQ has no long-poll — gateway or webhook only). | — | [`design-docs/channel-abstraction.md`](design-docs/channel-abstraction.md) · [spec 0003](product-specs/0003-multi-platform-channels.md) · [QQ-port plan](exec-plans/active/2026-06-10-channel-qq-port.md) · [platform APIs](references/messaging-platform-apis.md) |
| **Agent loop (ReAct)** | v1 shipped: seven-section prompt build, provider re-entry tool loop, scheduler-batched parallel dispatch (slice 120), trace rows, provider lifecycle hooks (126), profile-priced cost fill (129). | No open track-local slice; non-CLI conversation entry couples in downstream of a runtime owner driving channel receive/dispatch. | Routing seam shipped (slice 228); a runtime-service owner to drive it (Dependency Frontier #2). | [spec 0001](product-specs/0001-core-react-loop.md) · [spec 0017](product-specs/0017-fake-provider-first-agent-loop.md) · [`design-docs/agent-platform.md`](design-docs/agent-platform.md) |
| **Prompt framework (sections 1–7, cache)** | Stable: builder + promotion state (70–72), provider cache hints (73), default preamble owner (134), skill section 4 (135–149), memory section 5 (133, 162–167), per-agent overlay (141); stability bench + preamble grep gate in CI. | Changes ride the owning tracks (skills / memory / tools); new surfaces follow [`rules/prompt-design.md`](rules/prompt-design.md) "Adding A New Prompt Surface". | — | [`rules/prompt-design.md`](rules/prompt-design.md) · [spec 0016](product-specs/0016-prompt-and-tool-catalog-cache.md) |
| **Provider portability** | Anthropic Messages + OpenAI Responses, body + SSE streaming end-to-end in the binary (97–124), retry/fallback execution runtime, route/credential resolution, usage rollups (127). | Additional protocol families when a configured route needs one. | A concrete demand (config naming an unsupported protocol). | [`design-docs/api-portability.md`](design-docs/api-portability.md) |
| **Tools — registry & built-ins** | Seven filesystem/search built-ins + `tool.search` + memory/skill built-ins; structured output, usage counters, byte caps (spec 0014 closed for built-ins); shared `parse_input` helpers (115). | Re-shape `file.delete` + `directory.list` into one unified delete tool and a recursive whole-project list. | None hard; meanwhile do not add new per-kind splits. | [`design-docs/tool-runtime.md`](design-docs/tool-runtime.md) · [spec 0002](product-specs/0002-tool-registry.md) · [spec 0014](product-specs/0014-structured-tool-output.md) |
| **Tool scheduler** | Shipped (116–120): bounded parallelism, per-path read/write locks, per-call timeout, cancellation grace, production loop wiring. | Periodic `reap_idle_locks` tick from a long-lived runtime service. | Runtime-service owner (Dependency Frontier #2). | [spec 0012](product-specs/0012-tool-scheduler-and-state.md) |
| **Workspace & path policy** | v1 shipped (55): pre-resolution, confinement, redacted audit metadata. | v1.1 shared ignore predicate + display helpers. | A second recursive consumer — the recursive project list reshape (Dependency Frontier #4). | [spec 0013](product-specs/0013-workspace-and-path-policy.md) |
| **File-view & IO caching** | Shipped through 58: range reads, fingerprints, line-offset index, bounded caches, singleflight, inotify watcher. | Auto-start the watcher from bootstrap/config. | Runtime-service owner (Dependency Frontier #2). | [spec 0011](product-specs/0011-file-view-and-caching.md) · [`design-docs/io-runtime.md`](design-docs/io-runtime.md) |
| **Permissions & approvals** | Rules + materialization, signed approval broker with replay/TTL and per-identity grant caps (56), ask round-trip through the CLI operator sink (94–96). | External hook sink models (config `hooks.sinks` / `hooks.bindings` are accepted placeholders today). | Sink-model design in [`design-docs/permissions-and-hooks.md`](design-docs/permissions-and-hooks.md). | [spec 0008](product-specs/0008-permissions.md) |
| **Hooks & redaction** | Advisory parallel fan-out + blocking publish with timeout (91–92, 156), trust-tiered redaction (65, 152), shared payload snapshots (158), provider/memory lifecycle payloads (126, 179–180). | Blocking `memory_read_before` rewrite/veto consumer (currently deferred). | A consumer design — who rewrites recall queries. | [`design-docs/permissions-and-hooks.md`](design-docs/permissions-and-hooks.md) · [spec 0015](product-specs/0015-blocking-hook-decisions.md) |
| **Memory** | Session store + long-term FTS5 + gated sqlite-vec hybrid (`--vector_memory=y`), recall policy + `memory.recall/remember/forget` tools, read-touch, retention decay + leases (130–196). | Hybrid ranking policy/wiring + embedding ownership (tracker P1–P3); sqlite-vec corpus benchmarks. | Embedding-owner decision (Dependency Frontier #5). | [`design-docs/memory-system.md`](design-docs/memory-system.md) · [spec 0005](product-specs/0005-memory-system.md) |
| **Automation** | Library-level retention/cron/triggered with durable state, leases, queues, caller-owned finite loops (187–225, plus 183–186 retention prework); bootstrap intentionally does not open `automation.db` or run loops. | Service-loop/daemon ownership above `AutomationService::run(...)`; the cron-category plan is complete and archived. | Daemon / runtime-service owner decision (Dependency Frontier #2). | [`design-docs/automation-runtime.md`](design-docs/automation-runtime.md) · [spec 0006](product-specs/0006-automation.md) · [completed plan](exec-plans/completed/2026-06-07-automation-cron-category.md) |
| **Skills** | Full v1 arc (135–149): catalog renderer, loader + hot reload, `skill.invoke` / `skill.deactivate`, activation policy with config deactivation/expiration, durable session activations, event extraction. | Other runtime owners (channels / automation) persist and replay the same activation events. | Those runtime surfaces existing. | [spec 0009](product-specs/0009-skills.md) |
| **CLI** | Streaming render (123), interactive REPL (125), slash commands (128), operator approval sink (95). | Incremental growth as features demand; no open track-local slice. | — | [`design-docs/cli-runtime.md`](design-docs/cli-runtime.md) |
| **Desktop app** | Docs repivot from web UI to an in-process Slint app complete (plan archived); no `oran-desktop` code. | First `oran-desktop` slice (window + streaming view over the CLI event sink). | Slint package + compile-budget row; `web` → `desktop` config-block migration decision. | [spec 0007](product-specs/0007-web-ui.md) · [`DESKTOP.md`](DESKTOP.md) · [completed plan](exec-plans/completed/2026-06-06-replace-webui-with-desktop.md) |
| **Agent teams** | Design/spec only; no code. | First orchestration seam; channel routing shipped (slice 228), so design work can start. | A runtime owner for non-CLI entry helps but does not block the first seam (Dependency Frontier #2). | [`design-docs/team-collaboration.md`](design-docs/team-collaboration.md) · [spec 0004](product-specs/0004-agent-team.md) |
| **Observability & trace** | Spec 0018 v1: loop-owned trace rows for success/error/cancel/iteration-cap, `--trace` inspector (88), audit cause-chain join (79), usage rollups (127), retention purge (150). | Operator-facing rollup/report surface; observability is the lowest `QUALITY_SCORE` row (D). | — | [spec 0018](product-specs/0018-first-loop-observability.md) · [`RELIABILITY.md`](RELIABILITY.md) |
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
2. **Runtime-service / daemon owner** — one decision unblocks three tracks:
   automation service-loop ownership in bootstrap (or a headless
   `orangutan-server`), IO watcher auto-start, and the scheduler idle-lock
   reaping tick. Until then those stay caller-owned by design.
3. **Reference hardware for CI** — gates `check-compile-budget.sh` in
   `ci.sh` plus CI xmake build/test wiring (tech-debt rows 2026-05-20 /
   2026-05-21).
4. **Recursive list / unified delete reshape** — supplies the second
   recursive consumer that unblocks workspace v1.1's shared ignore
   predicate.
5. **Embedding ownership decision** — local deterministic embedding vs. a
   provider-backed one; gates hybrid ranking policy/wiring beyond the
   current `oran-local-text-v1`.
6. **Desktop docs-plan completion** — *resolved 2026-06-10*: the docs
   repivot plan is complete and archived; the first `oran-desktop` slice is
   gated only by the Desktop row's pre-dependencies (Slint package +
   compile-budget row, config-block migration decision).

## See Also

- [`STATUS.md`](STATUS.md) — point-in-time snapshot: current slice, last
  history, active exec-plans, library health, open tech-debt.
- [`histories/`](histories/) — canonical per-slice record (what/why/files).
- [`product-specs/index.md`](product-specs/index.md) — per-spec shipping
  status.
- [`exec-plans/tech-debt-tracker.md`](exec-plans/tech-debt-tracker.md) —
  open debt rows backing several pre-dependencies above.
- [`QUALITY_SCORE.md`](QUALITY_SCORE.md) — per-area health scores.
