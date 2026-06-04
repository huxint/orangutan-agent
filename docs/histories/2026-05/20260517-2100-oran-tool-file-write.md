## [2026-05-17 21:00] | Task: add `file.write` built-in on top of slice-17 `tool::Registry`

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code in local repository checkout`
- Linked plan: none — single-session slice that fits the
  `Next intended slice` bullet in
  [`STATUS.md`](../../STATUS.md) ("the remaining file built-ins
  (`file.write`, `file.edit`, `file.search`) on top of the slice-17
  `tool::Registry`"), matching `PLANS_GUIDE.md`
  "When NOT To Create A Plan".

### User Query

> 详细了解项目目标，查看当前项目真实进度, 继续推进项目代码的实现.
> (Understand the project goals, inspect the current real progress,
> continue advancing the project's code implementation.)

Slice 17 left the registry consuming the assembly's `AuditSink` with
exactly one built-in (`file.read`). The `Next intended slice` bullet
flagged the remaining three file tools — `file.write`, `file.edit`,
`file.search` — as the natural next-slice radar. This slice opens that
door with the minimum coherent unit: the `file.write` built-in. The
remaining two file tools plus the approval-broker wiring stay on the
next-slice radar.

### Changes Overview

- **New built-in `file.write`.** Registered via
  `tool::register_file_write` and aggregated into
  `tool::register_builtins`. Capability `Capability::write_file`.
  Schema: `{"path": <string>, "content": <string>, "mode"?:
  "truncate"|"append"|"fail_if_exists" (default truncate),
  "create_parents"?: bool (default false)}`,
  `additionalProperties:false`. Handler parses with `nlohmann::json`
  in its own TU, maps the optional `mode` onto `io::WriteMode`,
  forwards to `io::write_text_file`, and returns
  `Output{ text = "wrote N bytes to <path>" }` on success.
  Malformed input (bad JSON / missing path or content / wrong type on
  any field / unknown mode value) rejects as
  `Error::invalid_argument`. `fail_if_exists` collisions propagate
  from `oran-io` as `Error::conflict` unchanged.
- **`register_builtins` order.** `file.read` then `file.write` so the
  catalog is deterministic for any downstream consumer that snapshots
  it. The `register_builtins` test now asserts size 2 and the
  insertion-order pair.
- **`tests/tool` extension.** 7 new cases / 64 new assertions cover:
  - `register_file_write` advertises `Capability::write_file` and the
    multi-field schema.
  - Happy path writes the bytes verbatim and reports the size in the
    text confirmation.
  - Default mode overwrites an existing file.
  - `mode=append` concatenates to existing content.
  - `mode=fail_if_exists` returns `Error::conflict` and leaves the
    original file intact.
  - `create_parents=true` creates missing intermediate directories.
  - An eight-shot malformed-input table that covers bad JSON, missing
    path, missing content, non-string path, non-string content,
    non-string mode, unknown mode value (with the value carried in
    the error context), and non-bool create_parents — plus asserts
    each malformed call still recorded one `allow` audit row, proving
    the permission decision is independent of payload validity.
- **`bench-tool` extension.** New scenario file
  `bench/tool/scenarios/file_write.cpp` adds
  `file_write.dispatch_truncate` vs.
  `file_write.dispatch_append` — both write 64 bytes through the
  registry to the same tempfile. Measured ~12.4 µs vs. ~12.0 µs
  (within nanobench's ~3% noise band), confirming the IO-mode choice
  is not measurable at small payload sizes and would not be the
  fruitful axis for a future micro-optimisation. The existing
  `registry.lookup` (~8 ns) and `registry.dispatch_allow` (~2,000 ns)
  baselines are unchanged.
- **xmake plumbing.** `xmake/tests.lua` grew an optional
  `extra_packages` argument on the `oran_test` helper so `test-tool`
  can pull in `nlohmann_json` for the test-side JSON authoring
  (the production library keeps the package private — only the test
  bucket needs the header in its TUs).
- **Slice-version bump.** `kVersion` 17 → 18; the binary's CLI
  surface is unchanged. `xmake run orangutan --help` reports
  `orangutan v2.0.0-slice18`.

### Design Intent

**Why `file.write` is the right next-slice candidate.** It is the
top item in the `Next intended slice` list and the natural extension
of slice 17's registry + first built-in pattern. `io::write_text_file`
already exists with the matching `WriteMode` enum and
`create_parent_directories` option, so this slice does not pull in
any new IO machinery — it composes existing pieces. The remaining
two file tools (`file.edit`, `file.search`) each carry substantive
mechanism (patch-style diff with conflict detection;
ripgrep-style structured matches), so each warrants its own slice
under the C14 ≤ 600-line guideline.

**Why no workspace-confinement story in this slice.** The slice 17
history flagged "needs the workspace-confinement story" against
`file.write`. After reviewing the design doc, the conclusion is that
the workspace-confinement layer belongs in the future `tool::Runtime`
handle (the design doc's `runtime.workspace() -> Result<io::Workspace&>`
shape) — a cross-cutting concern that should apply uniformly to
`file.read`, `file.write`, `file.edit`, and `file.search`, not bolted
onto one of them. Slice 18 keeps parity with `file.read`: the path is
taken raw, and confinement is the permission rules' job (denying
`Capability::write_file` for paths outside the workspace via
`input_pattern` is already the supported mechanism today). When
`tool::Runtime` lands, both built-ins will pick up workspace
normalization at the same time.

**Why `mode` is a string, not a numeric enum on the wire.** Tools
talk to LLMs; an LLM produces JSON, and the readable form
`"truncate"` / `"append"` / `"fail_if_exists"` is what the schema
documents. The handler validates the string against the three known
spellings and returns the offending value in the error context on
mismatch, so a typo from the model is debuggable without reading
source.

**Why `create_parents` is opt-in.** Surprise directory creation is
the kind of side effect that should be explicit. Defaulting to
`false` keeps `file.write` consistent with the principle of least
surprise: an LLM asking to write to a path that doesn't exist gets a
clean `not_found` from `oran-io` and must explicitly request the
parent-creating shape.

**Why the success text quotes the byte count.** Returning an empty
text would force the agent loop to fabricate "I wrote your file" from
the permission decision alone. Including `wrote N bytes to <path>`
lets the model quote a concrete fact back to the operator with zero
parsing — the same pattern `file.read` follows where the file
contents *are* the text.

**Why one audit row, exactly as in slice 17.** The permission
decision is what audit records; the handler outcome is hook-bus
territory (still pending). Matching slice 17's "one row per
dispatch" invariant keeps the dispatch contract uniform — every
built-in now demonstrates that the registry alone is responsible
for the audit record, and the handler is responsible for the
*action*.

**Why bench `truncate_vs_append` instead of `dispatch_vs_direct`.**
The dispatch-vs-direct cost was already established by slice 17's
`registry.dispatch_allow` (~2 µs over an in-memory handler). The
informative new contrast for `file.write` is "does the mode choice
matter?" — and the measured answer ("no, both are ~12 µs dominated
by the disk write") is a documented baseline a future
implementation change would have to beat to justify itself.

### Files Modified

- `include/oran/tool/builtins.hpp` — new `kFileWriteName` constant
  and `register_file_write` declaration; refreshed module comment.
- `src/oran-tool/file_write.cpp` — new TU; handler + registrar.
- `src/oran-tool/builtins.cpp` — `register_builtins` now also wires
  `file.write` after `file.read`.
- `tests/tool/test_registry.cpp` — added `<nlohmann/json.hpp>` +
  `<iterator>` + `<system_error>` includes; new test helpers
  (`slurp`, `write_rule_set`); 7 new file.write test cases (+64
  assertions); the existing `register_builtins` test now asserts a
  catalog of `{file.read, file.write}`.
- `bench/tool/main.cpp` — registers the new
  `register_tool_file_write` block.
- `bench/tool/scenarios/file_write.cpp` — new scenario:
  truncate vs. append dispatch on a tempfile.
- `bench/tool/README.md` — documents the new A/B.
- `xmake/tests.lua` — `oran_test` gains an optional packages arg;
  `test-tool` calls it with `{ "nlohmann_json" }`.
- `src/oran-bootstrap/bootstrap.cpp` — `kVersion` 17 → 18.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 18, history pointer, library surfaces
  row for `oran-tool` (24 / 161), refreshed `Next intended slice`
  bullet (down to `file.edit` / `file.search` + approval-broker +
  Anthropic + signal shutdown).
- `docs/QUALITY_SCORE.md` — Tool registry row refreshed with the
  second built-in's surface, schema, and bench numbers; Test
  framework row refreshed with the new `oran-tool` counts; Bench
  harness row refreshed with the new `file_write` A/B numbers.
- `docs/ARCHITECTURE.md` — slice-status preamble bumped to
  2026-05-18 and now lists both file built-ins; `oran-tool`
  inventory row promoted from "first built-in `file.read`" to
  "built-ins `file.read` and `file.write`".
- `docs/design-docs/tool-runtime.md` — Status box (2026-05-18)
  notes both built-ins; `Capability::write_file` is now consumed in
  production code.
- `docs/releases/feature-release-notes.md` — new top row for
  `oran-tool-file-write`.
- `docs/histories/2026-05/20260517-2100-oran-tool-file-write.md` —
  this file.

### Validation

- Commands run:
  - `xmake build oran-tool` — clean (4 TUs, ~7.2 s incl. PCH rebuild).
  - `xmake build test-tool && xmake run test-tool` — 24 cases /
    161 assertions, all green.
  - `xmake test` — all 9 buckets green
    (test-tool joins test-core/async/io/storage/config/permission/
    cli/bootstrap).
  - `xmake build bench-tool && xmake run bench-tool` — clean;
    measured `registry.lookup ~8.16 ns`,
    `registry.dispatch_allow ~1,997 ns`,
    `file_write.dispatch_truncate ~12,371 ns`,
    `file_write.dispatch_append ~11,953 ns`.
  - `xmake build orangutan && xmake run orangutan -- --help` —
    prints the slice-18 banner; the CLI surface is unchanged.
- Tests added/changed: 7 new tool-bucket cases (+64 assertions).
- Bench impact: existing scenarios unchanged within noise; new
  `file_write` scenarios baselined above.
- Compile-budget delta: one new TU in `oran-tool`
  (`file_write.cpp` ~3 s for the nlohmann json include); one new
  TU in `bench-tool` (`file_write.cpp` scenario); the new
  `<nlohmann/json.hpp>` include in `tests/tool/test_registry.cpp`
  is amortised over a single TU and stays inside the bucket's
  existing budget envelope.

### Follow-ups

- Issues to file: none.
- Tech-debt entries: none added, none closed (the "remaining file
  built-ins" bullet trims to `file.edit` / `file.search`; the
  approval-broker mediation bullet is unchanged).
- Linked release note: 2026-05-17 `oran-tool-file-write` row in
  `docs/releases/feature-release-notes.md`.
- Cross-references for future agents: when the agent loop
  (`oran-agent`) lands, the natural extraction is the same as for
  slice 17 — wire `bootstrap::RuntimeAssembly`'s `AuditSink` + a
  `permission::RuleSet` built via `materialize_rules` into a single
  `tool::DispatchContext` shared by every iteration. Both file
  built-ins now live in the catalog. When `tool::Runtime` lands,
  both file built-ins should be migrated together onto the
  `runtime.workspace()` capability-gated handle so workspace
  confinement applies uniformly.
