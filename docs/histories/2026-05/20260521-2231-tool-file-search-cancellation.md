## [2026-05-21 22:31] | Task: file.search cancellation polling (deep-review P0)

### Execution Context

- Agent: Claude Code (Opus 4.7)
- Base model: claude-opus-4-7
- Runtime: interactive session driven by `orangutan-deep-review.md`
- Linked plan: none — single P0 item from `exec-plans/tech-debt-tracker.md`
  (`deep-review-2026-05-21`); see `PLANS_GUIDE.md` "When NOT To Create A Plan".

### User Query

> Continue moving the deep-review P0 backlog. Slice 32 closed BUG-4.1.1
> (atomic write to file.edit / file.write); SMELL-4.1.3 (file.search
> is uncancellable mid-walk) is next.

### Changes Overview

- Areas: `oran-tool::file.search` — `walk_and_scan` + `read_text_capped`
  now poll `asio::cancellation_state` once per directory entry and
  once per 8 KiB read chunk.
- Key actions:
  1. Add a small `is_cancelled(const asio::cancellation_state&)`
     helper at the top of `file_search.cpp`'s anonymous namespace —
     same pattern the io layer already uses, no new dependency.
  2. Pass `const asio::cancellation_state&` into `walk_and_scan`
     and `read_text_capped`. Inside the directory-iterator loop,
     poll once per entry (relaxed atomic load — single-digit
     nanoseconds, lost in the per-entry stat cost). Inside
     `read_text_capped`'s 8 KiB read loop, poll once per
     iteration so a multi-MiB file aborts within one chunk of the
     signal landing.
  3. Inside the directory walk, treat a `cancelled` error from
     `read_text_capped` as terminal rather than the existing
     "silently skip oversized/unreadable" path — otherwise a
     cancellation arriving during a single file's read would be
     swallowed and the walk would continue.
  4. The handler reuses the new helper for its two existing
     pre/post-hop checks.
  5. New regression test `file.search aborts a large-file read
     when cancellation fires mid-walk` in
     `tests/tool/test_registry.cpp` arms the polling by reading
     an 8 MiB file (~1024 chunk iterations) on a worker
     `std::jthread` while the test thread emits the signal after
     a 5 ms head start. The signal lands during the read; the
     next chunk-iteration poll surfaces it as
     `core::ErrorKind::cancelled`. Confirmed deterministic across
     10 consecutive runs on the dev box.

### Design Intent

The deep-review SMELL-4.1.3 footgun was concrete: a long-running
`file.search` (large tree or a pathologically large file) could
not respond to SIGINT or any other cancellation source until the
walk finished. The handler did check `cancellation_state` before
the executor hop and immediately after it, but once
`walk_and_scan` entered its synchronous loop the worker thread was
locked into the walk regardless of any subsequent signal.

The fix is the canonical asio pattern: thread a reference to the
cancellation state into the blocking work, poll periodically,
return `Error::cancelled()` when the state shows a non-`none`
cancellation. Polling at "once per directory entry" + "once per
8 KiB read chunk" matches the rule C11 guidance ("every async
function is cancel-aware … check periodically") without
introducing measurable overhead on the happy path (the load is a
relaxed atomic read, single-digit nanoseconds against
multi-microsecond per-entry stat cost).

The test deliberately exercises the *read-chunk* polling rather
than the *per-entry* polling because a single large file is
cheaper to set up than a directory of thousands of small files,
and because the per-chunk path is the harder bug to catch by
code review (an inattentive future agent could remove the poll
inside `read_text_capped` while keeping the per-entry poll, and
the per-entry test alone would not regress). The per-entry path
is exercised indirectly because the test directory contains the
one file and the walk visits it once.

Alternatives considered:

- **Skip the new test and rely on code review alone.** Rejected —
  the repo rule "every meaningful code change leaves the project
  with stronger verification than before" applies. A timing-based
  test that passes deterministically on the dev box is better
  than no regression signal, and if it becomes flaky on faster
  hardware the slice-29 / slice-31 history shows tech-debt rows
  are the right escape hatch.
- **Move `walk_and_scan` out of the anonymous namespace and test
  it directly.** Rejected — the anonymous namespace is the right
  encapsulation; exposing it for tests would invite agents to
  call it directly and bypass the handler's pre/post-hop
  cancellation checks. The integration-level test catches the
  same regression without surface drift.
- **Skip the inner read-chunk poll and only poll per directory
  entry.** Rejected — the multi-MiB-file pathological case is
  in the deep review's stated motivation ("a pathological
  pattern over a multi-GB tree is uncancellable"); polling per
  read chunk closes both halves of the surface for the cost of
  one atomic load per 8 KiB read.
- **Tackle more of the deep-review P0 surface in the same
  slice** (content-size caps on `file.write`/`file.edit`,
  transparent hashing on the registry, schema validation at
  `Registry::add`). Rejected — each is independent of
  cancellation polling, lives in a different file, and deserves
  its own history + test.

### Files Modified

- `src/oran-tool/file_search.cpp` — new `is_cancelled` helper;
  `walk_and_scan` and `read_text_capped` accept and poll a
  `cancellation_state` reference; walk treats a `cancelled`
  read result as terminal; handler uses the new helper.
- `tests/tool/test_registry.cpp` — added `<thread>` plus the
  four new asio includes (`bind_cancellation_slot`,
  `cancellation_signal`, `cancellation_type`, `co_spawn`,
  `executor_work_guard`); +1 case / +4 assertions covering the
  mid-walk cancellation pathway with `std::jthread` driving the
  io_context while the test thread emits the signal.
- `src/oran-bootstrap/bootstrap.cpp` — `kVersion` bumped
  `slice32` → `slice33`.
- `docs/design-docs/tool-runtime.md` — slice-33 status note on
  `file.search` cancellation polling.
- `docs/STATUS.md` — slice 32 → 33; `Last completed history`
  pointer; refreshed `oran-tool` test counts (89 → 90 cases,
  712 → 716 assertions).
- `docs/exec-plans/tech-debt-tracker.md` — strike the
  cancellation-polling bullet from the
  `deep-review-2026-05-21` P0 list.
- `docs/releases/feature-release-notes.md` — new top row for
  slice 33.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/tool-runtime.md` — slice-33 status note on
  the cancellation-polling fix.
- `docs/STATUS.md` — slice number, history pointer,
  `oran-tool` test/assertion counts.
- `docs/exec-plans/tech-debt-tracker.md` — cancellation-polling
  bullet struck from the deep-review P0 list.
- `docs/releases/feature-release-notes.md` — slice-33 entry.
- Earlier histories (e.g. the slice-32 row from this
  afternoon) are intentionally not changed — they chronicle
  past state, not current behavior.

### Validation

- Commands run:
  - `xmake f -m release`
  - `xmake build oran-tool test-tool` — both pass.
  - `xmake test` — 10 / 10 test targets pass.
  - `./build/linux/x86_64/release/test-tool '[cancellation]'`
    — 10 consecutive runs all pass (4 assertions / 1 case each
    time), no flakiness observed on the dev box.
  - `xmake build orangutan && xmake run orangutan` — prints
    `orangutan v2.0.0-slice33`.
- Tests added/changed:
  - `tests/tool/test_registry.cpp` — +1 case / +4 assertions
    (`file.search aborts a large-file read when cancellation
    fires mid-walk`). `test-tool` now reports 90 cases / 716
    assertions (was 89 / 712).
- Bench impact: untouched. The added polling is two relaxed
  atomic loads (one per entry, one per 8 KiB read chunk) —
  single-digit nanoseconds against the per-entry stat cost and
  per-chunk read cost. Re-bench once `oran-agent` lands a
  search-heavy workload.
- Compile-budget delta: `src/oran-tool/file_search.cpp` is
  unchanged in includes. `tests/tool/test_registry.cpp` gained
  `<thread>` (test-only — explicitly allowed by `rules/critical-rules.md`
  C2 "Enforcement: scripts/check-banned-includes.sh rejects new
  #include <thread> in non-test, non-bench code") plus four
  asio cancellation headers, all already pulled in transitively
  by `<asio/io_context.hpp>`. Will be picked up by the next
  `scripts/measure-tu.sh` run; no measurable budget impact
  expected.

### Follow-ups

- Issues opened: none — review file lives in-tree, future
  agents can consult it directly.
- Tech-debt entries: the `deep-review-2026-05-21` row in
  `exec-plans/tech-debt-tracker.md` loses the cancellation
  bullet; P0 still has content-size caps on
  `file.write`/`file.edit`, transparent hashing on the registry
  map, and schema validation at `Registry::add`.
- Linked release note:
  `docs/releases/feature-release-notes.md` gets the slice-33
  row (`tool-file-search-cancellation`).
