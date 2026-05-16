# AGENTS.md

The **harness-engineering** scaffold for **Orangutan v2** — a C++26 agent-runtime
rewrite of the original `orangutan/` project. *Agent-first*: every load-bearing
decision is recorded in a versioned file under `docs/` so that Claude Code,
Codex, or any future agent can ship features without depending on chat memory.

`AGENTS.md` is a **routing index**. The rules live in `docs/rules/`, the
architecture in `docs/design-docs/`, the product surface in `docs/product-specs/`,
and the current project state in `docs/STATUS.md`. This file points at them — it
does not restate them.

> **The Prime Directive — docs match reality.** A PR that changes behavior, build,
> config, dependencies, interfaces, file layout, commands, or conventions
> **must** update the matching docs in the same change. Canonical statement and
> change-type → docs-to-update table live in
> [`docs/rules/docs-in-sync.md`](docs/rules/docs-in-sync.md); the rule line is
> [`critical-rules.md#C16`](docs/rules/critical-rules.md).
> If a rule under `docs/rules/` cannot be honored, propose an edit to the rule
> first — do not silently break it.

---

## Read At The Start Of Every Task

| File | Why |
| --- | --- |
| `docs/STATUS.md` | **One-screen project snapshot.** Current slice, last completed history, active exec-plan, open tech-debt. Read first. |
| `docs/REPO_COLLAB_GUIDE.md` | Repository-wide working agreement (commit / PR / test expectations). |
| `docs/ARCHITECTURE.md` | Target architecture map, library boundaries, binary inventory. |
| `docs/design-docs/core-beliefs.md` | Non-negotiable operating principles. |
| `docs/rules/critical-rules.md` | Non-negotiable C++/build constraints. **Read before any code edit.** |
| `docs/rules/compile-budget.md` | Compile-time budget per TU and per target. The previous project failed here. |
| `docs/rules/docs-in-sync.md` | The Prime Directive: docs must match reality. What to update, when, how. |

## Read Before Finishing A Code Change

| File | Why |
| --- | --- |
| `docs/HISTORY_GUIDE.md` | When and how to record finished tasks under `docs/histories/`. |
| `docs/QUALITY_SCORE.md` | Current quality targets and gaps by area. |
| `docs/rules/testing-and-bench.md` | What counts as a passing change (tests **and** bench impact). |

## Module Routing — Read When The Task Touches A Module

Single index for both architecture (`design-docs/`) and rules (`rules/`). When a
task touches an area, read its row.

| Area | Design / spec | Rule |
| --- | --- | --- |
| Build / xmake / GCC 16.1 | `docs/BUILD_SYSTEM.md` | `docs/rules/module-and-pch.md` |
| Compile-time pressure | `docs/FAST_COMPILATION.md` | `docs/rules/compile-budget.md` |
| Async, executors, coroutines | `docs/design-docs/async-model.md` | `docs/rules/async-and-concurrency.md` |
| Error handling | — | `docs/rules/error-handling.md` |
| Tools / hooks / permissions | `docs/design-docs/tool-runtime.md`, `docs/design-docs/permissions-and-hooks.md` | — |
| Permissions surface | `docs/product-specs/0008-permissions.md` | — |
| Prompts / system prompt / tool catalog / skill body / cache discipline | `docs/design-docs/api-portability.md`, `docs/design-docs/tool-runtime.md`, `docs/product-specs/0009-skills.md` | `docs/rules/prompt-design.md` |
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
| Reliability / observability | `docs/RELIABILITY.md` | — |
| Security / supply chain | `docs/SECURITY.md`, `docs/SUPPLY_CHAIN_SECURITY.md` | — |
| CI/CD | `docs/CICD.md` | — |
| Third-party libraries | — | `docs/rules/libraries.md` |
| Code style (formatting, idioms, enums, ranges, contains) | — | `docs/rules/code-style.md` |
| Static analysis | — | `docs/rules/static-analysis.md` |
| Git / branch / PR workflow | — | `docs/rules/workflow.md` |
| External references | `docs/references/` | — |
| Legacy `orangutan/` lessons | `docs/references/orangutan-legacy-audit.md` | — |

## Conventions At A Glance

One-line summaries; the linked rule is canonical.

| Area | Convention | Rule |
| --- | --- | --- |
| Language | C++26 (`set_languages("c++26")`), GCC 16.1 baseline, Clang ≥ 19 secondary | [`critical-rules.md#C17`](docs/rules/critical-rules.md) |
| Console output | `std::print` / `std::println` / `std::format`; no `<iostream>` in `src/oran-*/` | [`critical-rules.md#C17`](docs/rules/critical-rules.md) |
| Algorithms / ranges | `std::ranges::*` over iterator-pair `std::*`; stdlib + in-repo helpers before hand-rolled loops | [`code-style.md` "Algorithms And Ranges"](docs/rules/code-style.md) |
| Membership tests | `std::ranges::contains` / `map.contains(k)` / `string::contains(sub)`, not `find != end()` / `find != npos` | [`code-style.md` "Membership Tests"](docs/rules/code-style.md) |
| Enums | `enum class` only; wire spelling via generic `core::enum_name` / `core::parse_enum`; no per-enum forwarding shims | [`code-style.md` "Enums"](docs/rules/code-style.md) |
| Static analysis | GCC 16.1 `-fanalyzer` via `xmake f --analyze=y` (opt-in for authors, mandatory in nightly CI) | [`static-analysis.md`](docs/rules/static-analysis.md) |
| Build | xmake; lock file pinned; PCH on, modules where supported, unity for cold modules | [`module-and-pch.md`](docs/rules/module-and-pch.md), [`BUILD_SYSTEM.md`](docs/BUILD_SYSTEM.md) |
| Async | standalone asio + C++20 coroutines; **no NVIDIA stdexec**; **no `std::thread`, no custom thread pool** | [`critical-rules.md#C2`](docs/rules/critical-rules.md), [`async-and-concurrency.md`](docs/rules/async-and-concurrency.md) |
| Error model | `core::Result<T>` (= `std::expected<T, Error>`) end-to-end; throwing wrappers only at named `main`-level shims | [`critical-rules.md#C3`](docs/rules/critical-rules.md), [`error-handling.md`](docs/rules/error-handling.md) |
| Logging | spdlog with `SPDLOG_FMT_EXTERNAL=1`; use `oran::log::*` shim, not raw spdlog macros | [`code-style.md` "Logging"](docs/rules/code-style.md) |
| JSON | `nlohmann::json_fwd.hpp` in headers; full include in `.cpp` only | [`critical-rules.md#C6`](docs/rules/critical-rules.md) |
| Strings | UTF-8 by contract; conversion handled at boundaries via `oran::core::str::*` | [`code-style.md` "Strings"](docs/rules/code-style.md) |
| Tests | Catch2 v3, one bucket per library, `tests/<lib>/...` | [`testing-and-bench.md`](docs/rules/testing-and-bench.md), [`critical-rules.md#C12`](docs/rules/critical-rules.md) |
| Benches | nanobench + Catch2 runners, one bucket per library, `bench/<lib>/...`; each bucket owns ≥ 1 A-vs-B comparison | [`testing-and-bench.md`](docs/rules/testing-and-bench.md), [`critical-rules.md#C12`](docs/rules/critical-rules.md) |
| Histories | required for every code-change task that modifies behavior | [`critical-rules.md#C13`](docs/rules/critical-rules.md), [`HISTORY_GUIDE.md`](docs/HISTORY_GUIDE.md) |
| Prompt design | system preamble / tool catalog / skill body live in stable `CacheSection`s ordered stable → dynamic; no clocks, IDs, or per-call state in the cached prefix; consult <https://github.com/Piebald-AI/claude-code-system-prompts> for prior art before designing a new prompt surface | [`prompt-design.md`](docs/rules/prompt-design.md) |

## Working Posture

- Prefer **small, explicit, repository-legible abstractions** over clever metaprogramming.
- **Compile time is a feature** — a clean build must stay under the budget in
  [`compile-budget.md`](docs/rules/compile-budget.md).
- **Hooks are pluggable, not magical** — every lifecycle point that *could* dispatch
  a hook is enumerated in [`permissions-and-hooks.md`](docs/design-docs/permissions-and-hooks.md).
- **No code is special** — every library has both `tests/` and `bench/` neighbours.
- For non-trivial work (multiple commits, migration risk, architectural impact) create
  an execution plan under `docs/exec-plans/active/` before writing code — see
  [`PLANS_GUIDE.md`](docs/PLANS_GUIDE.md).

## Quick Commands

```sh
make ci                  # docs + hygiene + STATUS.md freshness (pre-PR gate)
make new-plan SLUG=...   # scaffold an execution plan
make new-history SLUG=...# scaffold a history entry
make check-docs          # verify required docs exist
```

Once the C++ build is set up (per [`BUILD_SYSTEM.md`](docs/BUILD_SYSTEM.md)):

```sh
xmake f -m release && xmake build orangutan         # build the binary
xmake test                                          # run every test-* target
xmake build bench-<lib> && xmake run bench-<lib>    # run a benchmark
scripts/bench-compare.sh                            # cross-implementation benchmark report
```

## What This Repository Is Not

- It is **not** a fork of `orangutan/`. The legacy tree is reference material, audited in
  [`orangutan-legacy-audit.md`](docs/references/orangutan-legacy-audit.md). Do not copy
  code without justification.
- It is **not** a place for ad-hoc one-shot scripts. Anything reusable belongs under
  `scripts/` with the same lint rules as production code.
- It is **not** a place for free-form notes. If a piece of knowledge will outlive the chat,
  put it in `docs/`.
