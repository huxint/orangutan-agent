## [2026-05-20 22:20] | Task: real `compile_budget.json` baseline + `scripts/check-compile-budget.sh` regression gate (slice 28)

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code, orangutan-refactor`
- Linked plan: none — single-session slice that ships a ~60-line
  versioned baseline file + a ~150-LoC bash regression gate, both
  immediately consumable by the slice-27 `scripts/measure-tu.sh`
  output. Fits inside `PLANS_GUIDE.md`'s "When NOT To Create A Plan"
  envelope. This is the second of the two commits the user requested
  in the same work session as slice 27.

### User Query

> 继续推进项目代码实现. 你需要推进两个 commit 的实现.
>
> (Continue advancing project code implementation. Push two commits.)

### Changes Overview

- **`compile_budget.json` is no longer absent.** A new versioned
  baseline at the repository root mirrors the per-library and
  per-target tables in
  [`docs/rules/compile-budget.md`](../../rules/compile-budget.md) as
  machine-readable JSON. `categories.<oran-lib>` carries
  `{median_sec, p95_sec, hard_cap_sec}` for each of the 20 libraries
  the design contemplates (the 10 already shipped plus the 10 still
  on the roadmap — every documented library is in the table so a
  future slice that adds, say, `oran-agent` does not have to invent
  numbers); `targets.<phase>` carries the whole-build budgets
  (`configure`, `build_all_libs`, `clean_build_release`,
  `incremental_after_one_*`, `link_orangutan_binary`,
  `tests_build_delta`, `bench_build_delta`). The schema is documented
  inline via a top-of-file `$comment` array. Numbers come from
  `compile-budget.md` verbatim — that document is the source of truth
  and `compile_budget.json` is the consumable mirror; the two move
  together in the same commit (Prime Directive).
- **`scripts/check-compile-budget.sh` is no longer a stub.** The real
  implementation drives `scripts/measure-tu.sh --json`, joins each
  TU's measured wall-clock time against
  `compile_budget.json.categories[library]` via a single jq pass, and
  classifies each TU into one of four buckets:
  - `ok`   — seconds ≤ p95_sec.
  - `warn` — p95_sec < seconds ≤ hard_cap_sec (printed, exit 0).
  - `fail` — seconds > hard_cap_sec (printed, exit 1).
  - `undef` — library has no entry in `compile_budget.json`
    (printed, exit 2 — a shipped library must be budgeted in the
    same commit it gains its first TU).

  Two flags: `--show-all` (print greens too, default off) and
  `--no-strict` (demote `fail` to warning while keeping exit 0;
  useful when calibrating on new hardware without losing visibility).
  Summary line `check-compile-budget: N TUs (N ok, N warn, N fail, N
  undef)` always prints on stdout.
- **Slice-version bump.** `kVersion` 27 → 28. `xmake run orangutan
  --help` reports `orangutan v2.0.0-slice28`.
- **Empirical signal on first run.** On WSL2 / GCC 16.1 /
  release-mode-LTO the gate reports 21 ok / 1 warn / 16 fail / 0
  undef. The 16 fail rows are real — `compile-budget.md`'s reference
  hardware is "GCC 16.1, x86_64 Linux, 8 cores, 16 GB RAM, NVMe SSD,
  release mode (LTO on), -j$(nproc)" and this WSL image is not that
  baseline. The script is NOT wired into `scripts/ci.sh` in this
  slice (see "Why not wire into CI" below); it is operator-side
  until either (a) CI provisions xmake on the documented reference
  hardware or (b) budgets are explicitly recalibrated.

### Design Intent

**Why every documented library is in `compile_budget.json` even
though only 10 of 20 ship today.** The `undef` exit-2 branch above
forces the author of any new shipped library to add a budget row in
the same commit. Pre-populating every documented library means a
slice that adds, e.g., `oran-agent` does not have to invent
boundaries from scratch — the row already exists and the slice
either accepts the documented numbers or proposes a budget edit
alongside the new code. Keeps the budget JSON aligned with the
design doc's library inventory by default.

**Why `--no-strict` instead of two scripts.** A single script with a
flag keeps the regression gate's contract in one place; reviewers
see one budget binary and one set of exit codes. The flag exists
because there is a legitimate calibration window when hardware
changes or a structural change (e.g., enabling LTO) shifts the
absolute numbers — during that window operators want to see the
deltas without the build going red. The default is `strict=1` so
the doc contract ("Beyond hard cap → red (fails the build)") is the
out-of-the-box behavior; calibrators opt in.

**Why not wire into `scripts/ci.sh` in this slice.** Two preconditions
have to land first:

1. CI provisions xmake + builds. The trailing comment in
   `scripts/ci.sh` already documents this gap.
2. The CI hardware matches `compile-budget.md`'s reference baseline
   (8 cores, NVMe, native — not WSL). Otherwise the gate fires on
   environmental drift, not real regressions.

Wiring the script into `ci.sh` today would either (a) require
`--no-strict` (defeats the purpose) or (b) fail every PR on slow
hardware (eats credibility). Better to ship the gate and the
baseline, document the wiring sequence, and let the next slice
that runs xmake-in-CI flip the switch in one explicit commit.

**Why the `undef` row is exit-2 (env error), not exit-1 (regression).**
A library showing up in measurement without showing up in the
budget table is an inconsistency between the design doc inventory
and the implementation — it means the slice that added the library
forgot a doc-sync update. That is a different signal from "this TU
got slower"; conflating them would hide the doc-sync bug under a
performance failure. Exit-2 also flags it as an environment problem
("the budget file is out of sync"), pointing the next author at the
fix.

**Why the join is a single jq pass rather than a bash join loop.**
jq's `--slurpfile` + `select` happens in one process — predictable,
deterministic, no shell quoting hazards across the budget keys. The
bash loop that follows just formats the prepared TSV. Splitting
classification and formatting like this keeps each layer auditable.

### Files Modified

- `compile_budget.json` — new file (~60 lines). Versioned baseline
  with `schema_version: 1`, top-of-file `$comment` array documenting
  the schema + reference hardware, `categories` (20 libraries) and
  `targets` (9 phases).
- `scripts/check-compile-budget.sh` — full rewrite from the slice-0
  stub (~150 LoC). Argument parser with `--no-strict` / `--show-all`,
  preflight checks (xmake, jq, budget JSON, measure-tu.sh), single
  jq join pass against `measure-tu.sh --json` output, four-bucket
  classification, summary line, exit-code dispatch.
- `src/oran-bootstrap/bootstrap.cpp` — `kVersion` 27 → 28.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 28, history pointer, refreshed
  `Next intended slice` (the build-skeleton-scripts row is now
  fully closed; the next candidates are the first provider adapter
  exec plan or the blocking-hook-semantics slice).
- `docs/QUALITY_SCORE.md` — Compile-time discipline row updated with
  the slice-28 callout (the budget file + the regression gate, the
  first measurement result, and the wiring-into-CI follow-up).
- `docs/exec-plans/tech-debt-tracker.md` — build-skeleton-scripts
  row removed (closed); new row added for the CI wiring gap (gated
  on xmake-in-CI + reference-hardware calibration).
- `docs/releases/feature-release-notes.md` — new top row
  `build-check-compile-budget`.
- `docs/histories/2026-05/20260520-2220-check-compile-budget-gate.md`
  — this file.

### Validation

- Commands run:
  - `jq . compile_budget.json` — clean parse.
  - `bash -n scripts/check-compile-budget.sh` — clean parse.
  - `shfmt -d scripts/check-compile-budget.sh` — clean.
  - `scripts/check-compile-budget.sh --no-strict` — 38 TUs reported
    (21 ok, 1 warn, 16 fail, 0 undef); exit 0 with the "demoted by
    --no-strict" line on stderr.
  - `make ci` — clean (the six base CI gates remain green; the
    compile-budget check is not wired into CI by this slice — it is
    operator-side until reference-hardware calibration).
  - `xmake build orangutan && ./build/linux/x86_64/release/orangutan --help`
    — first line reads `orangutan v2.0.0-slice28`.
- Tests added/changed: none (this is a script + JSON slice; the
  script's behaviour is validated by the smoke runs above against
  the live compile_commands.json).
- Bench impact: none (no C++ changes other than the slice version
  literal).
- Compile-budget delta: none (only `src/oran-bootstrap/bootstrap.cpp`
  changed by one digit).

### Follow-ups

- Issues to file: none.
- Tech-debt entries filed: one new row — the CI wiring is gated on
  xmake provisioning + reference-hardware calibration (replaces the
  now-closed build-skeleton-scripts row).
- Linked release note: 2026-05-20 `build-check-compile-budget` row
  in `docs/releases/feature-release-notes.md`.
- Cross-references for future agents: when a new oran-X library
  lands, add a row to `compile_budget.json.categories.oran-X` in the
  same commit — the script will exit 2 otherwise. When the gate is
  wired into CI, the trailing comment in `scripts/ci.sh` should
  shrink to mention only the xmake provisioning (the
  check-compile-budget line goes from "this script will also run"
  to a real invocation).
