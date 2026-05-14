## [2026-05-14 23:49] | Task: `oran-async` slice 1

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: Codex CLI, repo on Linux/WSL2, GCC 16.1.1 system compiler.
- Linked plan: `docs/exec-plans/completed/2026-05-14-oran-async-slice-1.md`

### User Query

> Commit the current implementation, then continue project development at the
> `oran-async` slice: asio + coroutine Runtime + `Channel<T>` + cancellable
> `sleep_for`. After execution, review/check generated files for `.gitignore`,
> commit, and recommend the next task.

### Changes Overview

- Areas: `oran-async` library, xmake package/target wiring, tests/async,
  bench/async, async/build/test docs.
- Key actions:
  - Added `asio 1.36.0` to the xmake package lock and documented approval row.
  - Implemented `Runtime`, `RuntimeConfig`, `Awaitable<T>`, cancel-aware
    `sleep_for`, and bounded closeable `Channel<T>`.
  - Added `tests/async` coverage for runtime run/stop, timer completion,
    timer cancellation, channel FIFO, overflow, pending send, close/drain, and
    receive cancellation.
  - Added `bench/async` comparing direct coroutine post loop vs. capacity-1 channel
    handoff.
  - Updated the early `orangutan` greeting and target dependency so
    `xmake build orangutan` includes the async foundation.

### Design Intent

This is the smallest useful async foundation slice. `Runtime` owns asio state behind
a pimpl, while `Channel<T>` stays as a small template because typed queues need to
avoid type-erased payloads in the hot path. Mailbox policy stays out of this layer:
`Channel<T>` only provides bounded FIFO, close, overflow, and cancellation behavior;
orchestration decides later whether to drop, retry, or publish hook events.

### Files Modified

- `include/oran/async.hpp`, `include/oran/async/{awaitable_fwd,channel,runtime,sleep}.hpp`
- `src/oran-async/{runtime,sleep}.cpp`
- `tests/async/{main,test_async}.cpp`
- `bench/async/{README,main}.cpp`, `bench/async/scenarios/channel_ping_pong.cpp`
- `xmake/{packages,targets,tests,bench}.lua`, `xmake-requires.lock`
- `src/main.cpp`
- `docs/design-docs/async-model.md`, `docs/rules/async-and-concurrency.md`,
  `docs/rules/libraries.md`, `docs/rules/testing-and-bench.md`,
  `docs/rules/critical-rules.md`, `docs/rules/static-analysis.md`
- `docs/ARCHITECTURE.md`, `docs/BUILD_SYSTEM.md`, `docs/QUALITY_SCORE.md`,
  `docs/releases/feature-release-notes.md`, `tests/README.md`, `bench/README.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/ARCHITECTURE.md` — slice status and `oran-async` inventory row now match the
  shipped public surface.
- `docs/BUILD_SYSTEM.md` — package list, options, target helper, and PCH sample
  aligned with current xmake files.
- `docs/design-docs/async-model.md` — public signatures and slice-1 behavior for
  `Runtime`, `Channel<T>`, `sleep_for`, and tests.
- `docs/rules/async-and-concurrency.md` — timer cancellation rule references the
  shipped `sleep_for` helper and current async test style.
- `docs/rules/libraries.md` — `asio` version updated to 1.36.0, the version available
  in current xmake-repo.
- `docs/rules/testing-and-bench.md` and
  `docs/product-specs/0010-benchmark-harness.md` — async bench comparison updated
  to direct coroutine post loop vs. bounded channel handoff.
- `docs/QUALITY_SCORE.md` and `docs/releases/feature-release-notes.md` — slice-1
  status recorded.

### Validation

- Commands run:
  ```sh
  xmake f -m release -y
  xmake build oran-async
  xmake build orangutan
  xmake run orangutan       # reports v2.0.0-slice1
  xmake run test-async        # 8 cases / 38 assertions / all passed
  xmake run bench-async
  xmake test                  # test-core/default + test-async/default passed
  make ci
  scripts/check-lib-parity.sh
  git diff --check
  ```
- Tests added/changed: 8 async test cases under `tests/async/test_async.cpp`.
- Bench impact: new async A/B bucket. Latest local run:
  - `async.direct_post_loop`: 2,829.23 ns/batch
  - `async.channel_ping_pong`: 12,699.75 ns/batch
- Compile-budget delta: `oran-async` builds successfully; full gate numbers recorded
  in the final validation pass for this commit.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md` (2026-05 row).
- Next slice candidate: `oran-io` (async file IO/subprocess/signal wrappers) or
  `oran-storage` (expected-only SQLite core). `oran-io` is the cleaner next step if
  the goal is to exercise `Runtime`/`sleep_for` before database state lands.
