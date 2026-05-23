## [2026-05-24 02:45] | Task: Tool File Read Structured Output

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: `none — narrow spec-0014 built-in migration slice`

### User Query

Continue the long-running Orangutan v2 implementation by reading the project
docs first, moving one coherent version forward, keeping docs in sync, validating
the result, and committing it as its own version.

### Changes Overview

- Areas: `oran-tool` file-read built-in, structured tool output docs, release/status docs.
- Key actions: `file.read` now fills `Output::data_json` with a serialized
  `file_read` payload carrying the requested text, fingerprint, range, returned
  byte count, and truncation flag while preserving the spec-0011 text
  header/body fallback.
- Key actions: successful reads now also fill `Output::usage.bytes_read`,
  `files_touched=1`, and the truncation flag so future schedulers, hooks, and
  provider adapters do not have to parse prose to learn read cost.

### Design Intent

Spec 0014 migrates built-ins incrementally because the project does not yet have
provider adapters, scheduler caps, or audit fan-out consumers. `file.read` is the
right first structured-data caller after the base envelope because spec 0011
already pins the same file-view facts in the text header. Duplicating those facts
and the requested body in `data_json` gives structured-capable callers a stable
machine-readable shape while keeping the current model-facing text contract
unchanged.

### Files Modified

- `include/oran/tool/builtins.hpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `src/oran-tool/file_read.cpp`
- `tests/tool/test_registry.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice/version pointer, current structured-output summary,
  and `oran-tool` test/assertion count.
- `docs/ARCHITECTURE.md` — current `oran-tool` inventory and `file.read` row now
  describe `data_json` plus usage counters.
- `docs/design-docs/tool-runtime.md` — output-shape policy now records the
  shipped `file.read` JSON payload and remaining built-in migration work.
- `docs/product-specs/0011-file-view-and-caching.md` — file-view output status
  now reflects the `Output::data_json` payload.
- `docs/product-specs/0014-structured-tool-output.md` — migration status marks
  `file.read` as the first structured built-in payload.
- `docs/QUALITY_SCORE.md` — refreshed `oran-tool` assertion count and registry
  summary.
- `docs/releases/feature-release-notes.md` — user-visible release note.

### Validation

- Commands run:
  - `xmake run test-tool`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `xmake test`
  - `make ci`
  - `git diff --check`
- Tests added/changed: `tests/tool/test_registry.cpp` asserts whole-file,
  line-range, byte-range, and truncated `file.read` structured payloads plus
  usage counters.
- Bench impact: no new bench scenario; the change serializes already-available
  read metadata and requested text on the existing success path.
- Compile-budget delta: no public heavy include changes; JSON serialization stays
  in the `.cpp` handler and the public built-in comment remains stdlib-only.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`
