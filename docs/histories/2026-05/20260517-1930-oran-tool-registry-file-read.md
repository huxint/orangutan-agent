## [2026-05-17 19:30] | Task: introduce oran-tool with first built-in `file.read`

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code in local repository checkout`
- Linked plan: none — single-session slice that fits the
  `Next intended slice` bullet in
  [`STATUS.md`](../../STATUS.md) ("First tool built-ins consume the
  assembly's `AuditSink`"), matching `PLANS_GUIDE.md`
  "When NOT To Create A Plan".

### User Query

> 详细了解项目目标，查看当前项目真实进度, 继续推进项目代码的实现.
> (Understand the project goals, inspect the current real progress,
> continue advancing the project's code implementation.)

The slice-14 `RuntimeAssembly` plumbed a per-process
`permission::ApprovalBroker` + `permission::AuditSink`, but no caller
above the assembly was wired to *use* the sink yet. The
`Next intended slice` bullet flagged this — "first tool-registry
built-ins (`file.read`, `file.write`, `file.edit`, `file.search`)
plumbed through the assembly's `permission::AuditSink` so each call
records a row". This slice opens that door with the minimum coherent
unit: a brand-new `oran-tool` library, a `tool::Registry` that
dispatches through `permission::RuleSet::evaluate` + an
`AuditSink::record` call, and the first built-in `file.read`. The
remaining three file tools (`file.write`, `file.edit`, `file.search`)
plus shell tools and the hook bus / approval-broker integration stay
on the next-slice radar.

### Changes Overview

- **New `oran-tool` library** under `src/oran-tool/` +
  `include/oran/tool/`. Three TUs: `registry.cpp`, `file_read.cpp`,
  `builtins.cpp`; three public headers: `registry.hpp` (the registry +
  `Output` + `DispatchContext` + `Handler`), `builtins.hpp` (the
  free-function registrars), and the umbrella `tool.hpp`.
- **Registry surface.** `Registry::add` / `remove` / `find` /
  `catalog` / `dispatch`. `dispatch(name, input_json, ctx)` walks one
  call through (1) lookup, (2)
  `ctx.rules.evaluate(name, input_json, def.required_capabilities, ctx.mode)`,
  (3) one `permission::AuditEvent` recorded onto `ctx.audit` carrying
  the decision + `input_hash = SHA-256(input_json)`, (4) a verdict
  branch: `allow` → invoke the handler; `deny` → return
  `Error::permission_denied`; `ask` → return `Error::permission_denied`
  with `reason=approval_required` (the approval-broker flow lands in a
  later slice). Audit-sink errors are propagated, never swallowed.
- **`file.read` built-in.** Reads the workspace path under
  `Capability::read_file`. Parses input as JSON `{"path": <string>}`
  with `nlohmann::json` in the .cpp, calls
  `io::read_text_file(ctx.executor, path)`, returns the contents
  verbatim. Schema fields beyond `path` are rejected
  (`additionalProperties:false`).
- **`ToolDef::required_capabilities`.** The design doc's `requires`
  field is realized here under that name because `requires` is a
  reserved C++20 keyword. `ToolDef::with_no_input` and the existing
  member-wise equality keep working unchanged.
- **xmake targets.** New `oran-tool` library target in
  `xmake/targets.lua`, new `test-tool` and `bench-tool` in
  `xmake/tests.lua` / `xmake/bench.lua`, propagated to the `orangutan`
  binary's `add_deps`.
- **Slice-version bump.** `kVersion` 16 → 17 even though the binary's
  CLI surface is unchanged; the slice cadence is what STATUS tracks.
- **Tests** (new `tests/tool/`): 17 cases / 97 assertions covering
  add (rejects empty name / empty handler / duplicate), find
  (nullptr vs. pointer), catalog (insertion order), remove (and the
  second remove returning `not_found`), dispatch (unknown tool ->
  `not_found` with no audit row, allow path records one event +
  returns handler output + the `input_hash` matches
  `ApprovalAuthority::input_hash`, deny path records a deny row +
  returns `permission_denied`, ask path records an ask row + returns
  `permission_denied` with `reason=approval_required`, capability
  scope match-vs-miss under `Mode::strict`, sink-error propagation),
  `register_file_read` (advertises `Capability::read_file` and the
  path schema), `register_builtins` (catalog == `{kFileReadName}`),
  `file.read` happy path, malformed-input rejection (bad JSON /
  missing `path` / non-string `path`), and not-found path.
- **Bench** (new `bench/tool/`): A/B between `registry.lookup`
  (catalog-only `find`) and `registry.dispatch_allow` (full
  permission walk + recording-sink record + handler invocation).
  Measured ~7.77 ns lookup vs. ~2,048 ns dispatch_allow — the
  dispatch overhead is dominated by the libsodium SHA-256 input hash
  + the coroutine spawn + the audit record.

### Design Intent

**Why ship a registry + one built-in instead of all four file tools
at once.** The remaining three file tools each carry substantive
mechanism — `file.edit` needs patch-style diff application with
conflict detection, `file.search` wants ripgrep-style structured
matches, `file.write` needs the workspace-confinement story. Bundling
them into this slice would push the change well past the C14 600-line
guideline and dilute the design discussion. Landing the registry +
`file.read` first proves the dispatch contract end-to-end, gives the
agent loop something concrete to consume, and lets the next slice
focus the design conversation on each remaining tool's own semantics.

**Why a typed `DispatchContext` instead of the design doc's six
direct parameters.** The design-doc draft of `Registry::dispatch`
takes `permission::Evaluator&`, `hook::Bus&`, `Identity`, plus the
implicit executor, mode, scope_key, agent_key strings — six-plus
parameters today and growing. A typed struct keeps the `Handler`
signature stable; adding the hook bus or the approval broker later
is additive on the struct rather than a breaking change at every
call site. The struct uses references (non-copyable / non-movable);
callers brace-initialize one per dispatch.

**Why `string_view input_json` rather than `const nlohmann::json&`.**
Passing parsed JSON would force `<oran/tool/registry.hpp>` to include
`<nlohmann/json_fwd.hpp>` and pull every consumer into the json
header chain. Passing raw bytes keeps the registry header free of
the json include (matching the C6 boundary), lets the permission
evaluator's `input_pattern` match against exactly what the LLM sent,
and lets the audit row hash exactly what the LLM sent. Handlers
parse JSON in their own TU using their already-loaded json header.

**Why `Verdict::ask` short-circuits with
`reason=approval_required`.** The HMAC approval-broker work is
already on the next-slice radar and would dwarf this slice if folded
in. Recording `AuditOutcome::ask` and returning a typed
`permission_denied` keeps the audit pipeline faithful to the rule
decision while making the missing piece (operator UI + broker
mediation) impossible to miss — the upcoming slice will replace the
short-circuit with a `co_await broker.approve(grant, now)` step and
overwrite the outcome to `approved` / `rejected` before the audit
write.

**Why one audit row per dispatch (not one per phase).** The
permission `AuditEvent` vocabulary is *the permission decision*, not
the handler outcome. Recording the decision once captures everything
the audit semantics promise; per-handler diagnostics (latency,
success/error of the handler itself) belong to the hook bus, which
is a separate surface. Splitting now would conflate two concerns.

**Why `required_capabilities` rather than `requires`.** `requires`
is a reserved C++20 keyword (concepts grammar). The design doc
spells the field `requires`; this slice renames to
`required_capabilities` and updates the design doc's status box +
ARCHITECTURE.md so the in-code spelling is the canonical one going
forward. The intent — and the slot it occupies relative to
`Rule::capability` and `Mode::strict`'s default-deny — is unchanged.

### Files Modified

- `include/oran/core/tool_def.hpp` — new
  `required_capabilities: std::vector<Capability>` field; includes
  `<oran/core/capability.hpp>` and `<vector>`.
- `src/oran-core/tool_def.cpp` — `with_no_input` initialises the new
  field to `{}` to keep `-Wmissing-field-initializers` quiet.
- `include/oran/tool/registry.hpp` — new public header.
- `include/oran/tool/builtins.hpp` — new public header.
- `include/oran/tool.hpp` — new umbrella.
- `src/oran-tool/registry.cpp` — new TU.
- `src/oran-tool/file_read.cpp` — new TU.
- `src/oran-tool/builtins.cpp` — new TU.
- `tests/tool/main.cpp` — Catch2 entry point.
- `tests/tool/test_registry.cpp` — 17 new cases / 97 assertions.
- `bench/tool/main.cpp` — nanobench entry point.
- `bench/tool/scenarios/dispatch.cpp` — registry-lookup vs.
  dispatch_allow A/B.
- `bench/tool/README.md` — describes the A/B.
- `xmake/targets.lua` — new `oran-tool` library target + binary dep.
- `xmake/tests.lua` — new `test-tool` target.
- `xmake/bench.lua` — new `bench-tool` target.
- `src/oran-bootstrap/bootstrap.cpp` — `kVersion` 16 → 17.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 17, history pointer, library health row
  for `oran-tool`, latest library surfaces row for `oran-tool`,
  refreshed `Next intended slice` bullet.
- `docs/QUALITY_SCORE.md` — new "Tool registry" row (was C: design
  captured; now C: implementation exists with tests + bench),
  refreshed test/bench rows.
- `docs/ARCHITECTURE.md` — slice-status preamble; `oran-tool` row
  promoted from "planned" to "implemented (registry + `file.read`)".
- `docs/design-docs/tool-runtime.md` — Status box for
  `Capability` now mentions the registry consumes the vocabulary;
  `ToolDef::required_capabilities` spelling noted (replacing the
  design doc's draft `requires`).
- `docs/releases/feature-release-notes.md` — new top row.
- `docs/histories/2026-05/20260517-1930-oran-tool-registry-file-read.md`
  — this file.

### Validation

- Commands run:
  - `xmake build oran-tool` — clean (3 TUs, ~7.9 s including the
    PCH rebuild).
  - `xmake build test-tool && xmake run test-tool` — 17 cases /
    97 assertions, all green.
  - `xmake test` — all 9 buckets green (test-tool joins
    test-core/async/io/storage/config/permission/cli/bootstrap).
  - `xmake build bench-tool && xmake run bench-tool` — clean;
    measured `registry.lookup ~7.77 ns` vs.
    `registry.dispatch_allow ~2,048 ns`.
  - `xmake build orangutan && xmake run orangutan -- --help` —
    prints the slice-17 banner; the CLI surface is unchanged.
  - `scripts/check-lib-parity.sh` — passes (`tests/tool/` +
    `bench/tool/` exist).
  - `make ci` — passes (docs scaffold, repo hygiene, docs-sync,
    STATUS.md freshness, action pinning).
- Tests added/changed: 17 new tool-bucket cases (+97 assertions).
- Bench impact: new `bench-tool` bucket; baseline measurements
  recorded above.
- Compile-budget delta: new `oran-tool` library compiles in well
  under the existing per-library budget (3 TUs, the heaviest is
  `file_read.cpp` at ~3 s for the nlohmann json include).

### Follow-ups

- Issues to file: none.
- Tech-debt entries: none added, none closed (the "first tool
  built-ins consume the assembly's `AuditSink`" bullet stays on
  the radar pending `file.write` / `file.edit` / `file.search`
  and the approval-broker wiring).
- Linked release note: 2026-05-17 `oran-tool-registry-file-read`
  row in `docs/releases/feature-release-notes.md`.
- Cross-references for future agents: when the agent loop
  (`oran-agent`) lands, the natural extraction is to wire
  `bootstrap::RuntimeAssembly`'s `AuditSink` + a `permission::RuleSet`
  built via `materialize_rules` into a `tool::DispatchContext`
  shared by every iteration; the `Verdict::ask` short-circuit in
  `Registry::dispatch` is the obvious replacement point for the
  approval-broker flow.
