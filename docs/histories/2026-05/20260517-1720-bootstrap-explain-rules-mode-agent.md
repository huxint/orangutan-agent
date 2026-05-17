## [2026-05-17 17:20] | Task: plumb mode + per-agent selection into --explain-rules

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code, orangutan-refactor`
- Linked plan: none — single-session slice that fits the
  `Next intended slice` bullet in [`STATUS.md`](../../STATUS.md)
  ("pass mode + per-agent selection to `--explain-rules`"),
  matching `PLANS_GUIDE.md` "When NOT To Create A Plan".

### User Query

> 详细了解项目目标，查看当前项目真实进度, 推进项目代码的实现.
> (Understand the project's goals, inspect the current real
> progress, push the project's code forward.)

The slice-10 `--explain-rules` shipped with `Mode::default_`
hard-coded and an empty per-agent overlay; its history file
explicitly punted on the selectors. This slice closes that
follow-up so operators can sanity-check any mode against any
configured agent before the agent loop ever runs.

### Changes Overview

- **`oran-bootstrap` parses `--mode <name>` / `--mode=<name>`
  and `--agent <name>` / `--agent=<name>`.** A small
  `consume_value` helper centralises the `space-form` /
  `eq-form` dance; mode values pass through
  `core::parse_enum<permission::Mode>` so wire spellings stay
  generic (no per-enum forwarding shim).
- **Two new public functions on
  `<oran/bootstrap/bootstrap.hpp>`:**
  - `parse_explain_rules_selector(args) -> Result<ExplainRulesSelector>`
    parses the two flags out of the bootstrap arg vector and
    returns the resolved `{Mode, agent_name}`. Empty / unknown
    values surface as `Error::invalid_argument` with the
    offending flag attached.
  - `materialize_rules(cfg, selector) -> Result<permission::RuleSet>`
    runs `permission::materialize` against `cfg.permissions()`
    and the `cfg.agents()` entry matching `selector.agent_name`;
    an unknown agent name surfaces as `Error::not_found`. The
    empty-name path keeps slice-10 behavior (no overlay).
- **`run` consumes the parsed selector** when `--explain-rules`
  is set, prints the new header line
  `materialized rules (mode=<m>[, agent=<name>]): N total`, and
  exits zero on success.
- **`--help` text refreshed** to document the new flags and
  the legal mode spellings (`strict | default | permissive |
  sandboxed`). `kVersion` bumped 15 → 16.
- **Tests:** `tests/bootstrap/test_bootstrap.cpp` grows by
  13 cases covering selector parsing (defaults, every mode,
  eq-form, missing/empty values, unknown spelling), materialize
  behaviour (no agent, named overlay, mode-driven defaults,
  unknown agent), and end-to-end `run` paths (mode accepted,
  unknown mode rejected, unknown agent rejected). The bucket
  goes from 21 / 78 to 34 / 123.

### Design Intent

**Why expose `parse_explain_rules_selector` and
`materialize_rules` instead of testing through `run` and
stdout capture.** Two reasons. First, `run` writes to
`stdout` via `std::print`; intercepting that from a Catch2
test would mean either redirecting `stdout` (process-global,
unfriendly to parallel test runs) or refactoring the printer
to take a `FILE*`. Neither pulls its weight for a slice this
small. Second, the test wants to assert *which* rule set was
selected, not just that *some* rule set printed cleanly —
returning a typed `RuleSet` makes the assertion direct
(`has_rule_for_capability(rs, allow, egress_http)`).

**Why `--mode <name>` consumes `core::parse_enum<Mode>` rather
than a hand-rolled lookup table.** The repo's enum policy
(`docs/rules/code-style.md` "Enums") says generic reflection
beats per-enum shims; the `Mode` enum already has its wire
spellings stripped of trailing underscores by `enum_name` /
`parse_enum`, so `--mode default` parses to `Mode::default_`
without a special case. Adding a new mode in the future costs
zero CLI plumbing.

**Why surface unknown-agent as `not_found` instead of
`invalid_argument`.** The flag value *itself* is syntactically
fine (any non-empty string is a legal agent name); what fails
is the lookup against the loaded config. `Error::not_found`
matches the `load_config` precedent for "explicit path that
doesn't exist on disk" and keeps the error kinds semantic.
Tests pin both kinds so a future refactor that conflates them
will fail loudly.

**Why no new bench scenario.** The added path is one parse
loop (already O(args)) plus one linear `std::ranges::find_if`
over `cfg.agents()` (already O(agents)) plus a call into
`permission::materialize`, which has an A/B/C scenario in
`bench/permission/scenarios/`. The selector lookup has no
competing implementation worth comparing
(`docs/rules/testing-and-bench.md` "When To Benchmark":
*bench when you genuinely cannot rank impls by reading*). The
tests are the load-bearing verification.

**Why these helpers are public on the bootstrap header and
not internal to `oran-cli` or `oran-permission`.** The flag
surface is a bootstrap-level concern (it picks the mode the
*process* will explain), and the materialization is a thin
combiner that already lived as a private helper inside
`bootstrap.cpp`. Lifting it to the umbrella keeps the test
boundary clean without introducing a new library or a new
dependency arrow.

### Files Modified

- `include/oran/bootstrap/bootstrap.hpp` — new
  `ExplainRulesSelector` struct,
  `parse_explain_rules_selector`, `materialize_rules`; new
  include of `<oran/permission/rule_set.hpp>`.
- `src/oran-bootstrap/bootstrap.cpp` — argument parsing for
  `--mode` / `--agent`, `consume_value` / `matches_flag`
  helpers, `print_materialized_rules` takes the selector,
  new public function bodies, `kVersion` 2.0.0-slice15 →
  2.0.0-slice16, refreshed `--help` text.
- `tests/bootstrap/test_bootstrap.cpp` — 13 new test cases
  (+45 assertions) covering parsing, materialization, and
  end-to-end `run` paths; new
  `<oran/config/config.hpp>` and
  `<oran/permission/rule_set.hpp>` includes.
- `docs/STATUS.md` — slice 16, history pointer, refreshed
  bootstrap assertion count, refreshed `Next intended slice`
  bullet now that this candidate is closed.
- `docs/QUALITY_SCORE.md` — Bootstrap row (selectors live);
  Test framework row (bootstrap 34 / 123).
- `docs/ARCHITECTURE.md` — slice-status preamble + bootstrap
  row reflect the new flag surface.
- `docs/product-specs/0008-permissions.md` — Validation
  section gains the mode + agent example commands.
- `docs/design-docs/permissions-and-hooks.md` —
  "Diagnostics" paragraph mentions the new selectors.
- `docs/releases/feature-release-notes.md` — new top row.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice + history pointer + assertion
  counts + next-slice bullet.
- `docs/QUALITY_SCORE.md` — Bootstrap + Test framework rows.
- `docs/ARCHITECTURE.md` — slice-status preamble +
  bootstrap row.
- `docs/product-specs/0008-permissions.md` — Validation
  examples.
- `docs/design-docs/permissions-and-hooks.md` —
  diagnostics paragraph.
- `docs/releases/feature-release-notes.md` — new row.
- `docs/histories/2026-05/20260517-1720-bootstrap-explain-rules-mode-agent.md`
  — this file.

### Validation

- Commands run:
  - `xmake build oran-bootstrap` — clean.
  - `xmake build test-bootstrap && xmake run test-bootstrap` —
    34 cases / 123 assertions, all green.
  - `xmake test` — all 8 buckets green.
  - `xmake build bench-bootstrap` — clean (no API breakage).
  - `xmake build orangutan && xmake run orangutan -- --help` —
    prints the slice-16 banner with the refreshed `--explain-rules`
    line and the new `--mode` / `--agent` description.
  - `xmake run orangutan -- --explain-rules` —
    9 rules at `mode=default`.
  - `xmake run orangutan -- --explain-rules --mode permissive` —
    2 rules at `mode=permissive`.
  - `xmake run orangutan -- --config config.example.json
    --explain-rules --agent researcher` — 18 rules at
    `mode=default, agent=researcher`, including the
    researcher's `allow * capability=egress_http` overlay
    (`#17`).
  - `xmake run orangutan -- --config config.example.json
    --explain-rules --agent ghost` — exits 1 with
    `orangutan: not_found: --agent does not match a configured
    agent [agent=ghost]`.
- Tests added/changed: 13 new bootstrap cases (+45 assertions).
- Bench impact: none (the new path runs once per invocation
  and the underlying `permission::materialize` already has
  bench coverage).
- Compile-budget delta: `oran-bootstrap`'s public header now
  pulls in `<oran/permission/rule_set.hpp>` (which transitively
  brings in `<oran/permission/input_pattern.hpp>` and re2's
  *forward* declaration only — re2's actual header still stays
  inside `.cpp`s per rule C6). The bootstrap TU compiled in
  ~7.5 s clean.

### Follow-ups

- Issues to file: none.
- Tech-debt entries: none added, none closed (the "first
  tool-registry built-ins" and "first provider adapter" rows
  remain the next two candidates listed in `STATUS.md`'s
  "Next intended slice").
- Linked release note: 2026-05-17 `bootstrap-explain-rules-mode-agent`
  row in `docs/releases/feature-release-notes.md`.
- Cross-references for future agents: when `oran-bootstrap`
  owns the runtime mode selection for the *whole* process
  (not just the `--explain-rules` diagnostic), the selector
  parsing logic in `parse_args` is the natural extraction
  point — keep `parse_explain_rules_selector` as the test
  seam.
