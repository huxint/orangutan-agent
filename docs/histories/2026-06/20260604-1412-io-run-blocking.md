## [2026-06-04 14:12] | Task: Export `io::run_blocking`

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local CLI in `/home/huxint/projects/orangutan-refactor`
- Linked plan: none — this is a small tracker cleanup slice; the CI-wiring
  tracker item remains gated on native reference hardware, and this host is WSL2.

### User Query

> Deeply understand the project architecture and current progress, read the
> relevant documentation before further implementation, and start the next slice.

### Changes Overview

- Areas: `oran-io` public API, IO tests, slice status/docs.
- Key actions: added `<oran/io/blocking.hpp>` with public
  `io::run_blocking(executor, fn)`, re-exported it through `<oran/io.hpp>`,
  switched the private file-helper call sites to the public template, and added
  direct success/cancellation coverage.

### Design Intent

The deep-review tracker had kept the blocking boundary as a P2 cleanup item
because the same "post to the blocking executor, then run short synchronous IO"
shape should not be recopied by future tool/runtime callers. The exported helper
is intentionally narrow: it accepts a nullary callable returning
`core::Result<T>`, so cancellation can always surface as the repository-standard
`core::Error::cancelled` and the callable is never invoked after an already
cancelled parent coroutine.

### Files Modified

- `include/oran/io/blocking.hpp`
- `include/oran/io.hpp`
- `src/oran-io/file.cpp`
- `tests/io/test_file.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/io-runtime.md` — public API block and current IO status now
  describe `io::run_blocking`.
- `docs/ARCHITECTURE.md` — library inventory names the new public IO utility.
- `docs/exec-plans/tech-debt-tracker.md` — removes the closed P2
  `io::run_blocking` bullet from the deep-review row.
- `docs/STATUS.md` — slice/history pointer, IO test count, and open tech-debt
  summary refreshed.
- `docs/QUALITY_SCORE.md` — IO/test rows refreshed with the new test count.
- `docs/releases/feature-release-notes.md` — public API release note added.

### Validation

- Commands run:
  - `xmake build test-io`
  - `xmake run test-io` — 54 cases / 311 assertions.
  - `xmake build orangutan`
  - `xmake run orangutan -- --help` — reports `orangutan v2.0.0-slice154`.
  - `make ci`
- Tests added/changed: two direct `run_blocking` tests for result forwarding and
  pre-invocation cancellation.
- Bench impact: none; this exports an existing boundary and adds no competing
  implementation choice.
- Compile-budget delta: no budget thresholds changed; the new public template
  uses existing asio header slices already present in `oran-io`.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: the `io::run_blocking` P2 cleanup item is closed; CI xmake
  / compile-budget wiring remains gated on documented reference hardware.
- Linked release note: 2026-06-04 `io-run-blocking`.
