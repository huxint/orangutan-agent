# CLAUDE.md

**Orangutan v2** — a C++26 agent-runtime rewrite of the original `orangutan/`. *Agent-first*: every load-bearing decision lives in a versioned file under `docs/` so any agent can ship without chat memory.

This file is a **routing index**. Rules → `docs/rules/`. Architecture → `docs/design-docs/`. Product → `docs/product-specs/`. Current state → `docs/STATUS.md`. It points; it does not restate.

> **Prime Directive — docs match reality.** Any PR that changes behavior, build, config, deps, interfaces, layout, commands, or conventions **must** update the matching docs in the same change. Canonical statement: [`docs/rules/docs-in-sync.md`](docs/rules/docs-in-sync.md) ([`critical-rules.md#C16`](docs/rules/critical-rules.md)). If a rule can't be honored, propose editing the rule first.

---

## Read At The Start Of Every Task

| File | Why |
| --- | --- |
| `docs/STATUS.md` | Project snapshot: current slice, last history, active exec-plan, tech-debt. **Read first.** |
| `docs/REPO_COLLAB_GUIDE.md` | Commit / PR / test expectations. |
| `docs/ARCHITECTURE.md` | Library boundaries, binary inventory. |
| `docs/PRODUCT_SENSE.md` | Product principles — shape tradeoffs without re-asking. |
| `docs/design-docs/core-beliefs.md` | Non-negotiable operating principles. |
| `docs/rules/critical-rules.md` | Non-negotiable C++/build constraints. **Read before any code edit.** |
| `docs/rules/compile-budget.md` | Compile-time budget. The previous project failed here. |
| `docs/rules/docs-in-sync.md` | The Prime Directive. |

## Read Before Finishing A Code Change

`docs/HISTORY_GUIDE.md` · `docs/QUALITY_SCORE.md` · `docs/rules/testing-and-bench.md`

## Module Routing — Read The Row For The Area You're Touching

| Area | Design / spec | Rule |
| --- | --- | --- |
| Agent platform / vision (read before new top-level features) | `docs/design-docs/agent-platform.md` | — |
| ReAct loop / agent loop | `docs/product-specs/0001-core-react-loop.md`, `docs/product-specs/0017-fake-provider-first-agent-loop.md`, `docs/design-docs/agent-platform.md` | — |
| Bootstrap / config discovery / runtime assembly | `docs/design-docs/bootstrap-runtime.md` | — |
| CLI / terminal mode / prompt shell | `docs/design-docs/cli-runtime.md` | — |
| File I/O / directory listing / subprocess | `docs/design-docs/io-runtime.md` | — |
| Storage / SQLite / migrations / pool | `docs/design-docs/storage-runtime.md` | — |
| Build / xmake / GCC 16.1 | `docs/BUILD_SYSTEM.md` | `docs/rules/module-and-pch.md` |
| Compile-time pressure | `docs/FAST_COMPILATION.md` | `docs/rules/compile-budget.md` |
| Async, executors, coroutines | `docs/design-docs/async-model.md` | `docs/rules/async-and-concurrency.md` |
| Error handling | — | `docs/rules/error-handling.md` |
| Tools / hooks / permissions | `docs/design-docs/tool-runtime.md`, `docs/design-docs/permissions-and-hooks.md`, `docs/product-specs/0002-tool-registry.md`, `docs/product-specs/0014-structured-tool-output.md`, `docs/product-specs/0015-blocking-hook-decisions.md` | — |
| Tool scheduler / parallel tool calls / bounded runtime state | `docs/product-specs/0012-tool-scheduler-and-state.md`, `docs/design-docs/tool-runtime.md` (Scheduler Boundary) | `docs/rules/async-and-concurrency.md` |
| File-view system / range reads / fingerprints / caches | `docs/product-specs/0011-file-view-and-caching.md`, `docs/design-docs/io-runtime.md` (Future Slices) | — |
| Workspace + path policy (file-tool confinement) | `docs/product-specs/0013-workspace-and-path-policy.md`, `docs/design-docs/tool-runtime.md` (Workspace Handle) | — |
| Permissions surface | `docs/product-specs/0008-permissions.md` | — |
| Prompts / tool catalog / skill body / cache / approval text | `docs/design-docs/api-portability.md`, `docs/design-docs/tool-runtime.md`, `docs/design-docs/agent-platform.md`, `docs/product-specs/0009-skills.md`, `docs/product-specs/0014-structured-tool-output.md`, `docs/product-specs/0015-blocking-hook-decisions.md`, `docs/product-specs/0016-prompt-and-tool-catalog-cache.md`, `docs/product-specs/0017-fake-provider-first-agent-loop.md`, `docs/product-specs/0018-first-loop-observability.md` | `docs/rules/prompt-design.md` |
| Channels (QQ / Discord / Slack / Webhook / …) | `docs/design-docs/channel-abstraction.md`, `docs/product-specs/0003-multi-platform-channels.md` | — |
| LLM provider portability | `docs/design-docs/api-portability.md` | — |
| Memory (working / session / long-term / shared) | `docs/design-docs/memory-system.md`, `docs/product-specs/0005-memory-system.md` | — |
| Agent team collaboration | `docs/design-docs/team-collaboration.md`, `docs/product-specs/0004-agent-team.md` | — |
| Secrets / config / state | `docs/design-docs/secrets-and-state.md` | — |
| Module / TU boundaries | `docs/design-docs/module-boundaries.md` | `docs/rules/module-and-pch.md` |
| Skills | `docs/product-specs/0009-skills.md` | — |
| Automation (cron / periodic / triggered) | `docs/product-specs/0006-automation.md` | — |
| Web UI surface | `docs/product-specs/0007-web-ui.md`, `docs/FRONTEND.md` | — |
| Benchmark harness | `docs/product-specs/0010-benchmark-harness.md` | `docs/rules/testing-and-bench.md` |
| Reliability / observability | `docs/RELIABILITY.md`, `docs/product-specs/0018-first-loop-observability.md` | — |
| Security / supply chain | `docs/SECURITY.md`, `docs/SUPPLY_CHAIN_SECURITY.md` | — |
| CI/CD | `docs/CICD.md` | — |
| Third-party libraries | — | `docs/rules/libraries.md` |
| Code style | — | `docs/rules/code-style.md` |
| Static analysis | — | `docs/rules/static-analysis.md` |
| Git / branch / PR workflow | — | `docs/rules/workflow.md` |
| External references | `docs/references/` | — |
| Deep-review artifacts (naming, version stamp, delete-on-close) | — | `docs/rules/deep-review.md` |
| Legacy `orangutan/` lessons | `docs/references/orangutan-legacy-audit.md` | — |

## Working Posture

- Small, repository-legible abstractions over clever metaprogramming.
- Compile time is a feature — stay inside [`compile-budget.md`](docs/rules/compile-budget.md).
- Hooks are pluggable, not magical — all lifecycle points enumerated in [`permissions-and-hooks.md`](docs/design-docs/permissions-and-hooks.md).
- Every library has `tests/` and `bench/` neighbours.
- Exec plans are not the default — see [`PLANS_GUIDE.md`](docs/PLANS_GUIDE.md); `STATUS.md` always names the active plan or says `none` + why.
- Do not conflate prompt concepts: this file / `AGENTS.md` routes the development agent, while [`prompt-design.md`](docs/rules/prompt-design.md) governs prompt bytes Orangutan will emit at runtime. Deleted review artifacts and `/tmp/...` notes are provenance only after their findings are copied into specs or the tracker; see [`deep-review.md`](docs/rules/deep-review.md) for the lifecycle (version stamp on creation, delete-on-close).

## Quick Commands

```sh
make ci                   # docs + hygiene + STATUS.md freshness (pre-PR gate)
make new-plan SLUG=...    # scaffold an execution plan
make new-history SLUG=... # scaffold a history entry
make check-docs           # verify required docs exist
```

Once the C++ build is set up (see [`BUILD_SYSTEM.md`](docs/BUILD_SYSTEM.md)):

```sh
xmake f -m release && xmake build orangutan         # build the binary
xmake test                                          # run every test-* target
xmake build bench-<lib> && xmake run bench-<lib>    # run a benchmark
scripts/bench-compare.sh                            # cross-impl benchmark report
```

## What This Repository Is Not

- Not a fork of `orangutan/` — legacy is reference only ([`orangutan-legacy-audit.md`](docs/references/orangutan-legacy-audit.md)).
- Not a place for ad-hoc scripts — reusable code goes under `scripts/` with production lint rules.
- Not a place for free-form notes — knowledge that outlives the chat goes in `docs/`.

## Agent Skills

- **Issue tracker** — GitHub Issues via `gh`. See [`docs/agents/issue-tracker.md`](docs/agents/issue-tracker.md).
- **Triage labels** — canonical defaults. See [`docs/agents/triage-labels.md`](docs/agents/triage-labels.md).
- **Domain docs** — mapped onto this repo's framework (`docs/STATUS.md`, `docs/ARCHITECTURE.md`, `docs/design-docs/`, `docs/rules/`). See [`docs/agents/domain.md`](docs/agents/domain.md).
