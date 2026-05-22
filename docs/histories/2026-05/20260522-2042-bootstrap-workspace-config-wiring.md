## [2026-05-22 20:42] | Task: Bootstrap workspace ownership + config wiring

### Execution Context

- Agent: `Claude Code`
- Base model: `Opus 4.7`
- Runtime: `Claude Code CLI`
- Linked plan: none — scoped continuation of spec 0013

### User Query

> Deeply understand the project architecture and current implementation
> progress, continue advancing the project code, two slices, one commit per
> slice; ultrathink.

### Changes Overview

- Areas: `oran-config`, `oran-tool` (latent fix), `oran-bootstrap`,
  workspace/path policy, docs.
- Key actions: added typed `WorkspacePermissionsConfig` to
  `config::PermissionsConfig` and taught the loader to parse
  `permissions.workspace.extra_read_roots` and
  `permissions.workspace.extra_write_roots` as string arrays inside both
  the global `permissions` block and the per-agent overlay. Routed those
  arrays into a new `RuntimeAssemblyOptions::workspace_options`
  (`tool::WorkspaceOptions`) and made `bootstrap::RuntimeAssembly::build`
  construct and own a `tool::Workspace`, exposed via
  `RuntimeAssembly::workspace()`. `bootstrap::run` does the
  `config::WorkspacePermissionsConfig` → `tool::WorkspaceOptions` translation
  before calling `RuntimeAssembly::build`, so override roots canonicalise
  once at boot and a misconfigured root fails the boot rather than the
  first dispatched tool call. Bumped the version banner to slice 41.

### Design Intent

Spec 0013 had already closed the per-tool migration in slices 37-40; the
remaining work was structural — get `tool::Workspace` out of test helpers
and into the long-lived `RuntimeAssembly` bundle, and route the override
roots through `oran-config`. This slice lands both, which lets the next
agent-loop slice thread `RuntimeAssembly::workspace()` straight into
`DispatchContext::workspace` without inventing per-call construction.

Translating `config::WorkspacePermissionsConfig` into
`tool::WorkspaceOptions` lives in `bootstrap.cpp` so `oran-tool` keeps no
dependency on `oran-config`; the assembly only sees `tool::WorkspaceOptions`
and reuses the existing canonicalisation already inside
`tool::Workspace::create`.

Side effect: surfaced a latent bug in `canonical_directory` where
libstdc++'s `std::filesystem::status` sets `ec = ENOENT` for missing files
even though it also fills `file_type::not_found`. The pre-existing `if (ec)`
branch was hiding the intended `Error::not_found` branch. Fixed by tolerating
`std::errc::no_such_file_or_directory` so the existence check below decides
the verdict. This matters because `RuntimeAssembly::build` now relies on
that error kind to distinguish "operator pointed at a wrong directory" from
"directory exists but the FS is broken".

### Files Modified

- `include/oran/config/config.hpp`
- `src/oran-config/config.cpp`
- `include/oran/bootstrap/runtime_assembly.hpp`
- `src/oran-bootstrap/runtime_assembly.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `src/oran-tool/workspace.cpp`
- `xmake/targets.lua`
- `tests/config/test_config.cpp`
- `tests/bootstrap/test_runtime_assembly.cpp`
- `config.example.json`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 41, new history pointer, refreshed
  `oran-config` (24/171) and `oran-bootstrap` (48/153) test counts, narrowed
  remaining spec 0013 work to audit-metadata + pre-permission resolution.
- `docs/ARCHITECTURE.md` — `oran-config` and `oran-bootstrap` library
  inventory rows note the new typed surface and dependencies; the big
  slice-status block records slice 41.
- `docs/design-docs/tool-runtime.md` — Workspace Handle section + the
  slice-status block now describe bootstrap-owned workspace.
- `docs/product-specs/0013-workspace-and-path-policy.md` — Status section
  records slice 41; "Still pending" narrows to audit + pre-permission.
- `docs/SECURITY.md` — workspace-confinement paragraph names slice 41
  ownership; only audit metadata + pre-permission resolution remain.
- `docs/QUALITY_SCORE.md` — refreshed test counts.
- `config.example.json` — new `permissions.workspace` documentation stub
  with empty override arrays.
- `docs/releases/feature-release-notes.md` — user-visible release note.

### Validation

- Commands run:
  - `xmake build oran-config oran-bootstrap`
  - `xmake build test-config test-bootstrap test-tool`
  - `xmake test` (all 10 buckets pass)
- Tests added/changed:
  - `tests/config/test_config.cpp` adds five workspace cases (parse,
    env substitution, malformed entries, unknown fields warn/strict,
    agent overlay). `tests/config` now reports 24 cases / 171 assertions.
  - `tests/bootstrap/test_runtime_assembly.cpp` adds four workspace
    cases (root canonicalisation, override widening, missing root, missing
    extra root). `tests/bootstrap` now reports 48 cases / 153 assertions.
- Bench impact: no new bench scenario; the new field is data only and is
  consumed exactly once at boot.
- Compile-budget delta: not measured. Incremental rebuild of the affected
  TUs linked in well under a second on this environment; this is not the
  reference hardware gate.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: no new row. Remaining spec 0013 work (audit metadata
  + pre-permission resolution) stays inside the spec's Status section.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`
