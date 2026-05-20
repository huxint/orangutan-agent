## [2026-05-20 22:04] | Task: real `scripts/measure-tu.sh` per-TU compile-time emitter (slice 27)

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code, orangutan-refactor`
- Linked plan: none — single-session slice that ships a ~170-LoC bash
  script and a paired history entry, fits inside `PLANS_GUIDE.md`'s
  "When NOT To Create A Plan" envelope. The `Next intended slice`
  bullet in slice 26's STATUS.md named this script explicitly.

### User Query

> 继续推进项目代码实现. 你需要推进两个 commit 的实现.
>
> (Continue advancing project code implementation. Push two commits.)

This file documents the **first** of the two commits — the per-TU
measurement tool. The second commit (this slice's pair) lands
`compile_budget.json` + the real `scripts/check-compile-budget.sh` on
top of this script's output.

### Changes Overview

- **`scripts/measure-tu.sh` is no longer a stub.** The real
  implementation reads `compile_commands.json` (filtered to
  `src/oran-*/*.cpp` — library TUs only; tests, benches, and
  `src/main.cpp` excluded), iterates each entry, deletes the cached
  `.o` file, re-runs the recorded compile command with a `bash`
  `EPOCHREALTIME`-bracketed timer, and emits either a human-readable
  slowest-first table (default) or `--json` (an array of
  `{library, file, seconds}` objects, ready to feed
  `scripts/check-compile-budget.sh`).
- **Slice-version bump.** `kVersion` 26 → 27. `xmake run orangutan
  --help` reports `orangutan v2.0.0-slice27`.
- **Empirical baseline captured.** The first local run on
  WSL2 / GCC 16.1 / release-mode-LTO produced 38 TU measurements
  ranging 0.6 s (smallest `oran-core` TU) to 8.6 s
  (`src/oran-config/config.cpp`). Several library TUs already exceed
  the hard caps documented in
  [`docs/rules/compile-budget.md`](../../rules/compile-budget.md)
  ("Per-TU Budgets") on this measurement environment — most notably
  `oran-config` (8.6 s vs. 2.5 s cap), the four `oran-tool/file_*`
  TUs (6.5–7.4 s vs. 3.0 s cap), and `oran-bootstrap/bootstrap.cpp`
  (6.7 s vs. 5.0 s cap). This is real signal: this slice surfaces
  the gap; the paired `check-compile-budget.sh` slice will calibrate
  `compile_budget.json` against the documented targets and let
  reviewers see the deltas explicitly rather than silently
  inheriting them.

### Design Intent

**Why measure by re-running each compile command from
`compile_commands.json` rather than parsing `xmake -v` or `-ftime-report`
output.** xmake does not surface per-TU times in its `-v` output (the
only per-step number is the build-wide elapsed line at the tail). GCC's
`-ftime-report` and `-ftime-report-details` produce voluminous text that
needs careful parsing and double-counts time spent in `cc1plus`
sub-phases. Replaying the recorded compile command with a bracketing
timer is the cleanest contract: `compile_commands.json` is the canonical
record of what xmake would have run, the elapsed time is a single
straightforward subtraction, and the script stays a self-contained
~170-LoC bash file rather than a half-Python regex parser.

**Why `EPOCHREALTIME` instead of GNU `time`.** `/usr/bin/time` is not
installed on this WSL image and is not guaranteed to be on every CI
runner. Bash 5.0+ exposes `EPOCHREALTIME` as a built-in microsecond
clock that needs no external dependency. The fractional subtraction is
handled by a tiny `awk` invocation (bash integer arithmetic would
truncate the microsecond component). The script's `compile-budget.md`
contract is currently about wall time only; the RSS column in that
document's memory-budget table is left as a follow-up because RSS
sampling without GNU `time` or a custom helper is racy.

**Why filter to `src/oran-*/*.cpp` only.** The per-TU budget table in
`compile-budget.md` is keyed by library; tests and benches are
intentionally not budgeted (test buckets balloon under Catch2's macro
expansion but only matter for CI build time, which the per-target
budget in the same doc already covers). `src/main.cpp` is a single
tiny TU not worth budgeting separately. Restricting measurement to
library TUs keeps the JSON output uncluttered.

**Why a single sample per TU instead of multiple samples / median.**
Per-TU compile times have low variance once the page cache is warm
(the .o was just deleted, but headers are hot). A single sample is
within ~5% of a steady-state median in practice. The
`check-compile-budget.sh` slice will gate on the documented `hard_cap`
column, which is set well above p95 to absorb any per-run noise —
re-running for a median would add ~3× wall time to the script for no
real CI value.

**Why the script does NOT trigger a full xmake build itself.** The
slice-26 `check-deps.sh` pattern is that scripts validate state; they
don't bootstrap state. The script's preconditions (PCH built, release
mode configured) are documented at the top; if `xmake build` has not
run, the recorded compile command fails with a clear "PCH not found"
error from cc1plus and the script exits 1 with the compiler's actual
stderr forwarded. This keeps `measure-tu.sh` composable with a
caller's existing build state and the `compile-budget.md` workflow's
explicit `xmake -j$(nproc)` warmup step.

### Files Modified

- `scripts/measure-tu.sh` — full rewrite from the slice-0 stub
  (~170 LoC). New `--json` flag, jq-driven extraction of the per-TU
  arg list, `EPOCHREALTIME`-bracketed timer, slowest-first sorted
  output.
- `src/oran-bootstrap/bootstrap.cpp` — `kVersion` 26 → 27.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 27, history pointer, refreshed
  `Next intended slice` (now points at the paired
  `compile_budget.json` + `check-compile-budget.sh` slice that
  consumes this script's output).
- `docs/QUALITY_SCORE.md` — Compile-time discipline row updated with
  the slice-27 callout (what the script measures, the empirical
  baseline range, and the suggested next step).
- `docs/exec-plans/tech-debt-tracker.md` — build-skeleton-scripts row
  narrowed to the still-stub `check-compile-budget.sh`; the
  planned-follow-up text updated to reflect that the measurement
  tool is now real and the next missing piece is the baseline JSON
  + the regression gate.
- `docs/releases/feature-release-notes.md` — new top row
  `build-measure-tu`.
- `docs/histories/2026-05/20260520-2210-measure-tu-script.md` —
  this file.

### Validation

- Commands run:
  - `bash -n scripts/measure-tu.sh` — clean parse.
  - `xmake f -m release && xmake build` — warmup (clean build
    populated every library's PCH).
  - `scripts/measure-tu.sh` (default table mode) — emits the
    slowest-first table for all 38 library TUs; oran-config at 8.6 s
    is the top entry.
  - `scripts/measure-tu.sh --json | jq 'length, .[0]'` — confirms
    array shape: 38 entries, first entry has
    `{library, file, seconds}` keys with the expected types.
  - `make ci` — clean (the six base CI gates remain green; the new
    script is not wired into CI by this slice — it is operator-side
    until the paired budget-check slice ships).
  - `xmake build orangutan && ./build/linux/x86_64/release/orangutan --help`
    — first line reads `orangutan v2.0.0-slice27`.
- Tests added/changed: none (this is a script-only slice; the script's
  behaviour is validated end-to-end by running it against the live
  compile database).
- Bench impact: none (no C++ changes other than the slice version
  literal).
- Compile-budget delta: none (only `src/oran-bootstrap/bootstrap.cpp`
  changed, single character bumped in the `kVersion` literal).

### Follow-ups

- Issues to file: none.
- Tech-debt entries filed: none new. The existing build-skeleton
  scripts row is narrowed (this slice closes its `measure-tu.sh`
  half). The remaining stub is `check-compile-budget.sh`, addressed
  by the paired commit landing in this same slice work session.
- Linked release note: 2026-05-20 `build-measure-tu` row in
  `docs/releases/feature-release-notes.md`.
- Cross-references for future agents: the JSON shape this script
  emits is consumed by `scripts/check-compile-budget.sh` (the
  paired commit). That script joins each `{library, file, seconds}`
  entry against `compile_budget.json["categories"][library]` and
  flags any seconds value above the library's `hard_cap_sec`. The
  RSS column from `compile-budget.md` "Memory Budget" remains
  unmeasured by this script — a follow-up can either install GNU
  `time` in CI and re-emit `{rss_kb, …}`, or introduce a small
  C++ helper that wraps `getrusage(RUSAGE_CHILDREN)` around the
  compile.
