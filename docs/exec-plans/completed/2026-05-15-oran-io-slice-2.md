# `oran-io` — Slice 2

## Goal

Land the first platform I/O library for Orangutan v2. Slice 2 ships a small
`oran-io` MVP that provides cancel-aware coroutine wrappers for common text-file
operations and deterministic directory listing. The slice is intentionally below
tool/runtime policy: no permissions, hooks, subprocesses, signals, glob language,
or shell execution yet.

## Scope

- In scope:
  - Public headers under `include/oran/io/` plus umbrella `include/oran/io.hpp`.
  - `src/oran-io/file.cpp` implementing `read_text_file`, `write_text_file`, and
    `list_directory`.
  - `tests/io` coverage for read/write/list/error/cancellation behavior.
  - `bench/io` A-vs-B comparison for direct blocking read vs. the coroutine wrapper.
  - Promote the async test helper to `tests/test-helpers/` now that a second bucket
    needs it.
  - xmake target wiring and docs/history/release-note sync.
- Out of scope:
  - Subprocess, pipe, signal, and glob APIs.
  - Permission checks and hook dispatch; those belong to `oran-tool` / `oran-hook`
    and will wrap `oran-io` later.
  - Watchers / file-system notifications.
  - Platform-specific async file APIs such as io_uring. This slice uses standard
    library file I/O behind an executor hop.

## Context

- Relevant docs:
  - `docs/ARCHITECTURE.md`
  - `docs/design-docs/module-boundaries.md`
  - `docs/design-docs/async-model.md`
  - `docs/rules/async-and-concurrency.md`
  - `docs/rules/error-handling.md`
  - `docs/rules/testing-and-bench.md`
  - `docs/rules/docs-in-sync.md`
- Relevant code paths:
  - `xmake/{targets,tests,bench}.lua`
  - `include/oran/{core,async}/*`, `src/oran-{core,async}/*`
  - New `include/oran/io/*`, `src/oran-io/*`, `tests/io/*`, `bench/io/*`
- Constraints:
  - Public async APIs return `async::Awaitable<core::Result<T>>`.
  - Public headers stay stdlib-light and do not expose `<filesystem>` or stream
    types; path inputs are UTF-8 `std::string`.
  - Blocking standard-library file work happens only after an executor hop and
    checks cancellation before dispatch and before entering the blocking section.
  - File operations do not perform permission evaluation or hook publication yet.
- Compile-budget impact:
  - `oran-io` uses only stdlib + `oran-core`/`oran-async`; target category hard cap is
    2.0 s per TU. Implementation stays in one small `.cpp` for the MVP.

## Risks

- Risk: standard-library file operations cannot be interrupted once the OS call is in
  flight. Mitigation: document the cancellation boundary and test cancellation before
  the blocking section.
- Risk: file APIs accidentally become the future tool security boundary. Mitigation:
  keep this layer policy-free and record that permission/hook gating wraps it later.
- Risk: `std::filesystem` leaks into public headers and raises compile cost.
  Mitigation: use `std::string` path arguments in public headers and keep filesystem
  types in `src/oran-io/file.cpp`.

## Milestones

1. Add the active plan and design-doc entry for `oran-io`.
2. Implement the file/directory public surface and xmake wiring.
3. Add tests and an A-vs-B bench.
4. Update production docs, quality score, release notes, and history.
5. Run validation gates, review generated/ignored files, and move the plan to
   `completed/`.

## Validation

- Commands:
  - `make ci`
  - `xmake f -m release`
  - `xmake build oran-io`
  - `xmake build orangutan`
  - `xmake run orangutan`
  - `xmake run test-io`
  - `xmake test`
  - `xmake run bench-io`
  - `xmake f -m release --analyze=y`
  - `xmake build -r oran-io`
  - `xmake f -m release --analyze=n`
  - `scripts/check-lib-parity.sh`
  - `git diff --check`
- Manual checks:
  - `git status --short --ignored` shows only expected generated files ignored.
  - Public docs match the shipped function signatures.
- Observability checks: none yet; `oran-log` is not implemented.
- Bench comparison:
  - `bench/io` compares direct blocking text read vs. `io::read_text_file` through
    an asio coroutine wrapper.

## Progress Log

- [x] Confirm current state: slice 0 (`oran-core`) and slice 1 (`oran-async`) are
      complete; no active plan exists.
- [x] Select `oran-io` as slice 2 to exercise the async foundation before storage.
- [x] Implement the `oran-io` file/directory MVP.
- [x] Update docs that this slice invalidates in the same PR
      (`docs/rules/docs-in-sync.md`).
- [x] Run validation and record results.
- [x] Update `docs/QUALITY_SCORE.md`.
- [x] Write history entry.
- [x] Add release note.
- [x] Move this plan to `docs/exec-plans/completed/` before commit.

## Decision Log

- 2026-05-15: Slice 2 starts with file/directory helpers only. Rationale:
  subprocess/signal/glob semantics are security- and policy-adjacent, so they should
  land after the permission and hook surfaces are closer.
- 2026-05-15: Public paths are UTF-8 strings, not `std::filesystem::path`. Rationale:
  keep `<filesystem>` out of public headers while the repository is still defending
  compile-time budget.

## Linked Artifacts

- Related design doc: `docs/design-docs/io-runtime.md`
- Related product spec:
- PRs:
- History entry: `docs/histories/2026-05/20260515-2123-oran-io-slice-2.md`
- Release note: `docs/releases/feature-release-notes.md`
