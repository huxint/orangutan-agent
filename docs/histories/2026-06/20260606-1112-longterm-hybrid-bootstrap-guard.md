## [2026-06-06 11:12] | Task: Long-Term Hybrid Bootstrap Guard

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: local CLI, Asia/Shanghai
- Linked plan: none

### User Query

> Deeply understand the project architecture and current implementation progress,
> then start the next slice and commit it.

### Changes Overview

- Areas: `oran-bootstrap`, long-term memory docs, release/status tracking.
- Key actions: added a configured-route startup guard for
  `memory.longterm.hybrid_search.enabled`; when operators enable the policy
  before a vector-memory backend and embedding owner exist, bootstrap returns
  `ErrorKind::config` with the policy path and `reason=vector_memory_not_available`
  before opening runtime assembly state or sending a provider request.

### Design Intent

Slice 174 made the hybrid-search config shape valid, but the runtime still lacks an
owned `VectorBackend` and embedding source. Failing fast is the smallest honest
bootstrap consumption step: it prevents operators from enabling a no-op policy while
preserving the validated config contract for the later sqlite-vec / embedding slice.
The rejected alternative was silently falling back to lexical recall, because that
would make `enabled=true` misleading.

### Files Modified

- `src/oran-bootstrap/bootstrap.cpp`
- `tests/bootstrap/test_bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — moved the snapshot to slice 175 and recorded the guard.
- `docs/ARCHITECTURE.md` — documented the bootstrap/memory boundary update.
- `docs/design-docs/memory-system.md` — documented the guarded hybrid policy status.
- `docs/design-docs/bootstrap-runtime.md` — documented configured-route guard behavior.
- `docs/design-docs/secrets-and-state.md` — documented runtime handling for
  `hybrid_search.enabled=true`.
- `docs/product-specs/0005-memory-system.md` — recorded the shipped guard and
  refreshed bootstrap validation counts.
- `docs/QUALITY_SCORE.md` — refreshed bootstrap counts and status notes.
- `docs/releases/feature-release-notes.md` — added the user-visible fail-fast note.

### Validation

- Commands run:
  - `xmake build test-bootstrap`
  - `xmake run test-bootstrap "run rejects enabled memory hybrid search before vector memory exists"`
- Tests added/changed:
  - `test-bootstrap` now covers configured-route `hybrid_search.enabled=true`
    rejection, error context, absence of provider traffic, and absence of created
    `.orangutan` runtime state.
- Bench impact:
  - None; startup validation only.
- Compile-budget delta:
  - Not measured; one bootstrap TU and one bootstrap test TU changed.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  [`docs/releases/feature-release-notes.md`](../../releases/feature-release-notes.md)
