## [2026-05-20 20:30] | Task: real `scripts/check-deps.sh` CI gate (slice 26)

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code, orangutan-refactor`
- Linked plan: none — single-session slice that ships a ~120-LoC bash
  CI gate and fits the `PLANS_GUIDE.md` "When NOT To Create A Plan"
  envelope. The other candidates in slice 25's STATUS.md remained
  blocked (Anthropic Messages multi-slice, needs exec plan + `oran-http`
  first; blocking hook semantics needs the not-yet-existing
  `oran-agent`).

### User Query

> 继续完成项目下一个的实现.
>
> (Continue completing the next project implementation.)

### Changes Overview

- **`scripts/check-deps.sh` is no longer a stub.** The real
  implementation parses `xmake/targets.lua`, extracts each oran-X
  library's `add_deps(...)` token list (via `grep -oE` on the
  `"oran-..."` tokens inside each `oran_lib(...)` call), and validates
  every dep against a layering table that mirrors the six-layer
  hierarchy in [`docs/design-docs/module-boundaries.md`](../../design-docs/module-boundaries.md)
  ("Dependency Direction"): `foundation < platform < composition <
  agent-runtime < interface < composition-root`. Upward deps fail
  immediately. Same-layer (sibling) deps fail unless the
  `<dependent>-><dep>` pair appears in the script's `ALLOWED_SIBLING`
  allowlist, which mirrors the legitimate exceptions documented in
  `docs/ARCHITECTURE.md` and currently lists exactly five entries:
  `io->async`, `storage->async`, `config->storage`,
  `tool->permission`, `tool->hook`. Every failure prints a
  single-line, human-readable message naming both layers; the script
  exits non-zero on any failure.
- **Wired into `scripts/ci.sh`.** The check now runs in the base CI
  bundle alongside the docs, hygiene, docs-sync, status-freshness,
  and action-pinning gates. `make ci` exits non-zero on any
  dependency-direction violation, so a PR that adds an upward dep
  is caught before review.
- **Slice-version bump.** `kVersion` 25 → 26. `xmake run orangutan
  --help` reports `orangutan v2.0.0-slice26`.
- **Negative-test validation.** The script was exercised against a
  pristine `targets.lua` (exit 0, "dependency layering ok"), against a
  temp version with `oran-core` injected with a dep on `oran-bootstrap`
  (exit 1, "upward dep — oran-core (foundation) may not depend on
  oran-bootstrap (composition-root)"), and against a temp version
  with `oran-hook` adopting `oran-tool` as an undocumented sibling
  dep (exit 1, "undocumented sibling dep — oran-hook -> oran-tool
  (both composition)"). The pristine state was restored after each
  injection; the negative tests are not committed.

### Design Intent

**Why bash, not python.** The other `scripts/check-*.sh` gates are
all bash; keeping `check-deps.sh` in the same shape minimises CI
toolchain surface area (no Python interpreter needed for the base
CI bundle). Bash arrays + `grep -oE` are sufficient because
`xmake/targets.lua` declares every library via the `oran_lib(name,
{ deps }, ...)` helper — a single regex per line extracts both the
library name and the dep list. The complete grammar is one regex; a
Python parser would be heavier without buying anything the bash
shape misses.

**Why a hand-maintained `LAYER` table instead of parsing
module-boundaries.md.** The diagram in that doc is ASCII art for
human reading; programmatic extraction would either commit to a
specific markdown shape (fragile) or duplicate the table in a JSON
sidecar (the same hand-maintenance burden, just relocated). The
script's `LAYER` table is the canonical machine-readable copy of
the diagram; when the diagram changes, the table changes in the
same commit, and the CI gate catches the drift on its first run.

**Why a hand-maintained `ALLOWED_SIBLING` table instead of
parsing the ARCHITECTURE.md inventory.** Same logic, sharper edge:
`ARCHITECTURE.md`'s inventory is prose ("Depends on (allowed)"
column with comma-separated lib names mixed with package names like
`asio`, `sqlite3`, `nlohmann_json`). Distinguishing oran-X deps
from package deps in that column would require a multi-step parse
that adds bugs without adding value. The `ALLOWED_SIBLING`
allowlist is the same five entries as the live xmake state; when a
sixth legitimate sibling dep lands, the PR adds one line to the
allowlist and one line to ARCHITECTURE.md in the same commit.

**Why the script catches "unknown library" rather than tolerating
new targets silently.** A new `oran-X` library is a real
architectural event — it picks a layer, declares its deps, and
shows up in the ARCHITECTURE.md inventory. Letting the script
silently skip an unrecognised target means a typo'd or
not-yet-classified library lands with zero validation. Forcing the
author to add a `LAYER[<name>]=<n>` entry in the same commit that
adds the xmake target keeps the gate honest.

**Why `bootstrap` is a sixth "composition-root" layer rather than
the top interface layer.** `bootstrap` depends on `oran-cli` (the
interface layer), which would be an upward dep under the
five-layer diagram. The project's reality is that `bootstrap` is
the composition root that wires the entire process together — it
sits above every other library because it constructs all of them.
Encoding that as a sixth layer above interface keeps the rule
"strictly downward" intact without per-library exceptions.

### Files Modified

- `scripts/check-deps.sh` — full rewrite from the slice-0 stub (~120
  LoC). New `LAYER` table (22 entries, six layers including the
  `bootstrap` composition root), `LAYER_NAME` for human-readable
  failure text, `ALLOWED_SIBLING` allowlist (five entries), single
  `while` loop over `xmake/targets.lua`'s `oran_lib(...)` calls
  with a regex extractor for the dep tokens.
- `scripts/ci.sh` — new invocation of `check-deps.sh` between
  `check-status-fresh.sh` and the optional `check-action-pinning.sh`.
- `src/oran-bootstrap/bootstrap.cpp` — `kVersion` 25 → 26.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 26, history pointer, refreshed `Next
  intended slice` (now points at `measure-tu.sh` as the next
  unblocked script), tech-debt list narrowed to drop the two
  scripts that are now real.
- `docs/QUALITY_SCORE.md` — Build system row updated with the
  slice-26 callout (the gate, what it validates, and how it's
  wired).
- `docs/exec-plans/tech-debt-tracker.md` — build-skeleton-scripts
  row narrowed to the two still-stub scripts (`measure-tu.sh`,
  `check-compile-budget.sh`); the planned-follow-up text updated
  to reflect the suggested next ordering.
- `docs/releases/feature-release-notes.md` — new top row
  `build-check-deps`.
- `docs/histories/2026-05/20260520-2030-check-deps-script.md` —
  this file.

### Validation

- Commands run:
  - `scripts/check-deps.sh` against current state — exit 0, prints
    `check-deps: dependency layering ok`.
  - `scripts/check-deps.sh` against a temp `targets.lua` with
    `oran-core` injected with a dep on `oran-bootstrap` — exit 1,
    prints the upward-dep message.
  - `scripts/check-deps.sh` against a temp `targets.lua` with
    `oran-hook` injected with a dep on `oran-tool` — exit 1,
    prints the undocumented-sibling message.
  - `make ci` — clean (all six base CI gates green, including the
    new `check-deps`).
  - `xmake build orangutan && ./build/linux/x86_64/release/orangutan --help`
    — first line reads `orangutan v2.0.0-slice26`.
- Tests added/changed: none (this is a script-only slice). The
  negative tests above were one-shot manual injections; they verify
  the script's two failure paths.
- Bench impact: none (no C++ changes).
- Compile-budget delta: none (only `src/oran-bootstrap/bootstrap.cpp`
  changed, single character bumped in the `kVersion` literal).

### Follow-ups

- Issues to file: none.
- Tech-debt entries filed: none. The remaining open rows are:
  advisory-only hook bus (gated on first blocking consumer in
  `oran-agent`); `file.search` ripgrep-class optimisations (gated on
  a real workload measurement); `scripts/check-prompt-preamble` (gated
  on first stable preamble in `oran-agent`); `prompt_cache_hit_rate.cpp`
  bench scenario (gated on `oran-agent`); the still-stub `measure-tu.sh`
  + `check-compile-budget.sh`; generated `config.schema.json` (gated
  on broader config models); bench A-vs-B placeholders; frontend
  stack choice.
- Linked release note: 2026-05-20 `build-check-deps` row in
  `docs/releases/feature-release-notes.md`.
- Cross-references for future agents: the next build-skeleton script
  in line is `scripts/measure-tu.sh` per the tech-debt entry's
  planned-follow-up text — it emits per-TU compile times as JSON,
  which then feeds `compile_budget.json` (a versioned baseline) and
  `scripts/check-compile-budget.sh` (the regression gate). The
  shape sketched in [`rules/compile-budget.md`](../../rules/compile-budget.md)
  "Mechanical Checks" is the canonical reference. When that lands,
  this slice's `ALLOWED_SIBLING` pattern is a useful template: the
  measure-tu allowlist of "expected slow TUs" is a parallel idea —
  small hand-curated table that the script consults to distinguish
  legitimate slow TUs (e.g. coroutine-heavy headers) from
  regressions.
