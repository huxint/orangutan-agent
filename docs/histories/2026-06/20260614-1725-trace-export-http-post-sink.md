## [2026-06-14 17:25] | Task: Trace export HTTP POST sink

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none; this is a small unblocked spec-0018 Observability v1.1
  slice.

### User Query

> Continue iterating the project slice-by-slice after reading the development
> docs, choose the most valuable current slice, keep docs synchronized with
> reality, and close the slice with a detailed conventional commit.

### Changes Overview

- Areas: `oran-bootstrap` trace export, bootstrap tests, Observability docs.
- Key actions: added `--trace-export-post <url>` as a mutually-exclusive sink
  beside stdout and `--trace-export-file <path>`; reused the existing redacted
  JSON Lines payload for single-turn and bounded list export; POSTs as
  `application/x-ndjson` through `oran-http::Client`; treats 2xx responses as
  success and non-2xx responses as IO errors with the status code; pinned
  argument validation and loopback HTTP behavior in `test-bootstrap`.

### Design Intent

Spec 0018 v1.1 already calls for stdout, file, and `oran-http` POST export
before broader query-language work. The slice deliberately keeps export
selection in bootstrap instead of adding a new abstraction: the payload is
already materialized as redacted JSON Lines, the file sink can share that string
builder, and the HTTP sink only needs a one-shot `BodyRequest` with an explicit
operator URL. File and POST sinks are mutually exclusive so operators get one
delivery path per command and stdout suppression stays deterministic.

### Files Modified

- `src/oran-bootstrap/bootstrap.cpp`
- `tests/bootstrap/test_bootstrap.cpp`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - bumped slice, history pointer, latest slice summary,
  next intended Observability slice, and bootstrap test counts.
- `docs/ROADMAP.md` - advanced the Observability frontier to slice 242 and set
  the next unblocked spec-0018 item to the SQL-derived tool-call rollup.
- `docs/product-specs/0018-first-loop-observability.md` - marked the HTTP POST
  sink shipped in v1.1 and AC13, including validation command shape.
- `docs/RELIABILITY.md` - documented the operator-facing POST export behavior,
  content type, and non-2xx handling.
- `docs/design-docs/bootstrap-runtime.md` - documented bootstrap ownership of
  stdout/file/POST trace export without overstating POST signal semantics.
- `docs/ARCHITECTURE.md` - updated the `oran-bootstrap` inventory and added the
  current trace-export sink note.
- `docs/QUALITY_SCORE.md` - refreshed bootstrap test counts and the
  Observability/Bootstrap rows.
- `docs/releases/feature-release-notes.md` - added the user-visible release
  note for the HTTP POST sink.
- `README.md` - added a quick-start example for `--trace-export-post`.

### Validation

- Commands run:
  - `git diff --check`
  - `xmake build test-bootstrap`
  - `build/linux/x86_64/release/test-bootstrap "[trace]" --reporter=console --verbosity=normal`
  - `build/linux/x86_64/release/test-bootstrap --reporter=console --verbosity=normal`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `make ci`
- Tests added/changed: `test-bootstrap` now covers POST argument validation,
  single-turn POST payloads, bounded-list POST payloads, stdout suppression, and
  non-2xx failure reporting.
- Bench impact: none; this is one-shot operator export, not a hot loop.
- Compile-budget delta: no new public headers or templates; bootstrap adds one
  `oran-http` include and one one-shot client call path.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`.
