## [2026-05-21 00:45] | Task: deep-review rank-0 fixes — Channel races + Bus exception swallow + hook event count

### Execution Context

- Agent: Claude Code (Opus 4.7)
- Base model: claude-opus-4-7
- Runtime: interactive session driven by `orangutan-deep-review.md`
- Linked plan: none — four targeted point fixes; see `PLANS_GUIDE.md` "When NOT To Create A Plan".

### User Query

> Read `orangutan-deep-review.md` (full architecture + implementation review)
> and act on it. The reviewer flagged four "rank-0" items — three latent
> correctness bugs in the async/hook foundation plus a Prime-Directive
> doc-drift item — as the most important findings in the report. Land them
> as a focused first slice; defer the larger P0/P1 surface for a follow-up.

### Changes Overview

- Areas: `oran-async::Channel` (cancellation race + handler-executor bug),
  `oran-hook::Bus::publish_advisory` (exception swallow), production docs
  (`38 events` → `41 events`), slice version bump, regression tests.
- Key actions:
  1. Mirror the `storage::Pool::async_acquire_writer` discipline in
     `Channel::async_send` / `Channel::async_receive`: install the
     cancellation slot **after** the waiter is enqueued and under the same
     mutex, so a cancellation that fires during the gap finds the waiter
     to cancel instead of scanning an empty queue.
  2. Capture `asio::get_associated_executor(handler, executor_)` inside
     `make_send_complete` / `make_receive_complete` so completions resume
     on the handler's bound executor — typically a `strand` — instead of
     silently being posted to the channel's executor and racing other
     strand-bound work.
  3. Wrap the per-sink `co_await sink->receive(...)` in `publish_advisory`
     with a try/catch that converts any escaping exception into
     `Error::internal(...).with("sink", id)` and continues the loop, so
     tool dispatch's `[[maybe_unused]] auto after_outcome = co_await ...`
     publish can no longer crash on a throwing extension.
  4. Update `docs/ARCHITECTURE.md`, `docs/QUALITY_SCORE.md`, and
     `docs/design-docs/permissions-and-hooks.md` so the running event
     count matches the enum in `include/oran/hook/event.hpp` (41:
     5 agent + 4 provider + 4 tool + 6 memory + 6 channel +
     7 orchestration + 4 automation + 2 session + 3 permission).
     History files and the slice-22 release-note entry are intentionally
     left alone — they are chronicles of past state, not descriptions of
     current state.

### Design Intent

The review's rank-0 list is correctness-first: each item silently corrupts
agent behavior under conditions that only show up under contention or
extension misbehavior. We took the four together because they share one
pattern (the codebase has the *right* template for each — Pool's cancel
discipline, asio's associated-executor contract, the documented advisory
"never abort the publish" rule — and the affected sites just hadn't
adopted it), and the doc drift is the same shape (the enum grew, the
prose didn't follow). Landing them as one focused slice protects the
about-to-land `oran-agent` loop, which is the first consumer of all three
surfaces.

Alternatives considered:

- **Defer the Channel cancellation race fix until a test could deterministically
  reproduce it.** Rejected — the race is shaped to manifest only under
  multi-threaded executor + concurrent cancellation, which is exactly the
  regime `oran-agent` will introduce. Landing the structural fix now is
  cheaper than landing it after the agent loop starts depending on the
  current shape.
- **Tackle more of the P0 / P1 surface in the same slice** (atomic-write
  to `file.edit`, content-size caps, transparent hashing on the registry
  map). Rejected — those are independent of the rank-0 items, would have
  pushed this PR past the ~600 LoC / ~6 file size guideline, and each
  deserves its own history + tests. Tech-debt-tracker will gain a row
  cross-referencing the deep-review file for the remaining items.

### Files Modified

- `include/oran/async/channel.hpp` — added `<asio/associated_executor.hpp>`;
  rewrote `async_send` / `async_receive` to install the cancellation slot
  inside the mutex after the push (with a docstring linking to
  `storage::Pool`); rewrote `make_send_complete` / `make_receive_complete`
  to capture `asio::get_associated_executor(handler, executor_)` instead
  of `executor_`.
- `src/oran-hook/bus.cpp` — added `<exception>`; wrapped the per-sink
  `co_await` body in try/catch with `Error::internal` capture + `.with("sink", id)`
  context; advisory contract now matches the header's documented promise.
- `tests/async/test_async.cpp` — new regression case "Channel resumes
  coroutine on its associated executor (not the channel's)" that wires a
  receive on `strand_b` while sending from `strand_a` and asserts the
  receiver wakes on `strand_b`.
- `tests/hook/test_bus.cpp` — new `ThrowingSink` (raises `std::runtime_error`
  from its awaitable) + regression case asserting a throwing sink does
  not abort the publish, is captured as `Error::internal` with `sink=<id>`
  context, and the middle sink between two throwers still runs.
- `src/oran-bootstrap/bootstrap.cpp` — `kVersion` bumped `slice30` → `slice31`.
- `docs/ARCHITECTURE.md` — `38 lifecycle events` → `41 lifecycle events` in
  the slice-22 status block and the `oran-hook` library row; the row also
  notes that sink exceptions are caught and surfaced as `Error::internal`
  in `PublishOutcome` (so the contract documentation matches the now-true
  behavior).
- `docs/QUALITY_SCORE.md` — Hooks row: `38 enumerators` → `41 enumerators`
  and a new clause noting the slice-31 exception-catch hardening.
- `docs/design-docs/permissions-and-hooks.md` — Bus status block re-dated
  to 2026-05-21 / slice 31 and the count updated `38` → `41`.
- `docs/STATUS.md` — slice 30 → 31, `Last completed history` pointer,
  refreshed `oran-async` (8 → 9 cases / 38 → 43 assertions) and
  `oran-hook` (14 → 15 cases / 79 → 97 assertions) library surfaces.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/ARCHITECTURE.md` — event-count fix in the slice-22 status block
  + library row; sink-exception contract noted on the `oran-hook` row.
- `docs/QUALITY_SCORE.md` — event-count fix + slice-31 callout on the
  Hooks row.
- `docs/design-docs/permissions-and-hooks.md` — Bus status block re-dated
  and event count updated.
- `docs/STATUS.md` — slice + history pointer + per-library test counts.
- `docs/releases/feature-release-notes.md` — new top row for slice 31
  (see "Validation" below — this is the user-visible row).
- History files (e.g. `docs/histories/2026-05/20260518-1100-...`) and the
  slice-22 release-note row intentionally **not** changed — those describe
  what slice 22 *shipped*, which was 38 events; subsequent slices added
  three more without updating the prose.

### Validation

- Commands run:
  - `xmake f -m release`
  - `xmake build test-async test-hook` — both pass
  - `xmake build orangutan` — passes; `xmake run orangutan` now reports
    `2.0.0-slice31`
  - `xmake test` — all 10 test targets pass
  - `xmake run test-async` — 43 assertions in 9 cases (was 38/8)
  - `xmake run test-hook` — 97 assertions in 15 cases (was 79/14)
- Tests added/changed:
  - `tests/async/test_async.cpp` — +1 case / +5 assertions (handler
    associated-executor regression).
  - `tests/hook/test_bus.cpp` — +1 case / +18 assertions (throwing sink
    advisory contract + `ThrowingSink` helper).
- Bench impact: untouched — no perf claims affected. The new try/catch
  in `publish_advisory` is a per-iteration overhead estimated <10 ns on
  the happy path (zero-cost when no exception escapes); pre-existing
  `bench-hook/publish_one_sink` ~446 ns sets the order-of-magnitude
  context.
- Compile-budget delta: `oran-async/channel.hpp` gained one include
  (`<asio/associated_executor.hpp>`) but the header is already pulled
  in transitively by `<asio/io_context.hpp>` in every TU that touches
  channels; no measurable budget impact expected. The `oran-hook/bus.cpp`
  TU gained `<exception>` (system header, near-zero cost). Will be
  picked up by the next `scripts/measure-tu.sh` run.

### Follow-ups

- Issues opened: none — review file lives in-tree, future agents can
  consult it directly.
- Tech-debt entries: a new row in `exec-plans/tech-debt-tracker.md`
  to track the **deferred** items from the deep review, grouped by
  priority band (P0: atomic-write to `file.edit`, content-size caps,
  transparent hashing on registry, JSON schema validation at
  `Registry::add`, cancellation polling in `file.search` walk; P1: see
  the review §6 P1 table; …). Each row points back at the relevant
  line range in `orangutan-deep-review.md` so future agents can pick
  one and land it without re-deriving the analysis.
- Linked release note: `docs/releases/feature-release-notes.md` gets
  the slice-31 row (`rank-0-correctness-fixes`).
