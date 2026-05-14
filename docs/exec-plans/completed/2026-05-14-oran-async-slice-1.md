# `oran-async` — Slice 1

## Goal

Land the first asynchronous runtime library for Orangutan v2: standalone `asio`
as the executor substrate, `Runtime` as the bootstrap-owned io/cpu executor wrapper,
`Awaitable<T>` as the public coroutine vocabulary, a bounded `Channel<T>` for
cross-coroutine backpressure, and a cancel-aware `sleep_for` helper. The slice also
adds the `tests/async` and `bench/async` buckets required by the library-parity rule.

## Scope

- In scope:
  - Add the `asio 1.36.0` package to `xmake/packages.lua` and keep
    `docs/rules/libraries.md` in sync.
  - Add public headers under `include/oran/async/` plus the umbrella
    `include/oran/async.hpp`.
  - Add `src/oran-async/` implementation files for `Runtime` and cancellable timers.
  - Implement a bounded, closeable `Channel<T>` with async `send`, `receive`, and
    non-blocking `try_send`.
  - Wire `oran-async` into xmake, with `tests/async` and `bench/async`.
  - Update async docs, build docs, quality score, release notes, and history.
- Out of scope:
  - `oran-io`, `oran-http`, `oran-agent`, channel adapters, provider execution.
  - Production signal handling in bootstrap; `Runtime::stop()` is enough for this
    slice.
  - Full benchmark baseline JSON and CI bench comparison automation.

## Context

- Relevant docs:
  - `docs/design-docs/async-model.md`
  - `docs/rules/async-and-concurrency.md`
  - `docs/design-docs/module-boundaries.md`
  - `docs/rules/module-and-pch.md`
  - `docs/rules/testing-and-bench.md`
  - `docs/rules/docs-in-sync.md`
- Relevant code paths:
  - `xmake/packages.lua`, `xmake/targets.lua`, `xmake/tests.lua`, `xmake/bench.lua`
  - `include/oran/_pch.hpp`, `include/oran/core/*`
  - new `include/oran/async/*`, `src/oran-async/*`, `tests/async/*`, `bench/async/*`
- Constraints:
  - Public async functions return `async::Awaitable<core::Result<T>>`.
  - No `<thread>` / `std::thread`; use asio executors only.
  - Keep heavy asio includes out of non-async public headers.
  - `Channel<T>` is templated and necessarily header-defined, but it must stay small
    and use a minimal include set.
- Compile-budget impact:
  - `oran-async` has a 1.0 s median / 2.0 s p95 / 2.5 s hard cap per TU. Asio
    headers are heavier than `oran-core`, so this slice keeps implementation split
    into small TUs and avoids spreading asio outside `oran-async`.

## Risks

- Risk: asio package naming/version drift in xmake-repo. Mitigation: pin the documented
  `asio 1.36.0`; if xmake resolves a different shape, update the package and docs in
  the same change.
- Risk: channel cancellation semantics are easy to overbuild. Mitigation: this slice
  supports close/cancel delivery via asio cancellation-aware waits and tests the
  externally visible behavior; mailbox-specific drop policy lands later.
- Risk: public headers pull too much asio into downstream libraries. Mitigation:
  `awaitable_fwd.hpp` is the advertised lightweight include; heavy channel/runtime
  headers are opt-in.

## Milestones

1. Create the active plan and update package/build wiring.
2. Implement the async public surface and runtime/timer TUs.
3. Implement tests for runtime execution, timer cancellation, channel send/receive,
   channel close, and overflow.
4. Add a bench scenario comparing bounded `Channel<T>` coroutine ping-pong against a
   direct coroutine post-loop baseline.
5. Update production docs, quality score, release notes, and history.
6. Run review, generated-file/.gitignore check, validation gates, then commit.

## Validation

- Commands:
  - `make ci`
  - `xmake f -m release`
  - `xmake build orangutan`
  - `xmake run test-async`
  - `xmake test`
  - `xmake run bench-async`
  - `scripts/check-lib-parity.sh`
  - `git diff --check`
- Manual checks:
  - `git status --short --ignored` shows only expected ignored generated files.
  - Public docs match the shipped signatures.
- Observability checks: none yet; `oran-log` is not implemented.
- Bench comparison:
  - `bench/async` records bounded-channel ping-pong vs. direct coroutine post loop on
    this machine. No historical baseline exists yet.

## Progress Log

- [x] Confirm scope and constraints from the user-provided objective:
      `oran-async` = asio + coroutine `Runtime` + `Channel<T>` + cancellable
      `sleep_for`.
- [x] Add package/build wiring.
- [x] Implement public surface and source files.
- [x] Add tests and bench.
- [x] Update docs that this slice invalidates in the same PR
      (`docs/rules/docs-in-sync.md`).
- [x] Run final validation and record results.
- [x] Update `docs/QUALITY_SCORE.md`.
- [x] Write history entry.
- [x] Add release note.
- [x] Move this plan to `docs/exec-plans/completed/` before commit.

## Decision Log

- 2026-05-14: `Channel<T>` is a small template in a public header. Rationale:
  typed queues must support arbitrary payloads without type erasure in the hot path.
  Consequence: this header includes selected asio headers, but only consumers that
  explicitly include `channel.hpp` pay that cost.

## Linked Artifacts

- Related design doc: `docs/design-docs/async-model.md`
- Related product spec:
- PRs:
- History entry: `docs/histories/2026-05/20260514-2349-oran-async-slice-1.md`
- Release note: `docs/releases/feature-release-notes.md`
