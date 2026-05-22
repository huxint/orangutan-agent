## [2026-05-23 00:10] | Task: `expected_version` contract on `file.write` and `file.edit` (closes spec 0011 v1)

### Execution Context

- Agent: `Claude Code`
- Base model: `Opus 4.7`
- Runtime: `Claude Code CLI`
- Linked plan: none — scoped to spec 0011 v1 acceptance criterion 4.

### User Query

> Deeply understand the project architecture and current implementation
> progress, continue advancing the project, two slices, one commit per
> slice; ultrathink.

### Changes Overview

- Areas: `oran-tool` (`file.write` + `file.edit` mutation guard), a
  new private `oran-tool` header that the three file built-ins share,
  `tests/tool`, version banner, the docs describing the new wire
  surface.
- Key actions:
  - Lifted the slice-45 `version_token(canonical_path,
    FileFingerprint)` helper out of `file_read.cpp` into a new
    private header `src/oran-tool/version_token.hpp`. The header sits
    under `src/` (not `include/oran/tool/`) because the token shape is
    an implementation detail of the built-in catalog — agents treat
    the token as opaque and never spell it themselves. Future
    built-ins (the spec-0011 v2 `file.modify` shape, the `code.*`
    family) consume the same helper rather than re-deriving the
    wire format.
  - `src/oran-tool/file_write.cpp`: schema grows
    `expected_version: string`. Handler parses the field after the
    other options and, when it is set, runs a synchronous
    `io::compute_file_fingerprint(resolved_path)` and compares to
    the supplied token before doing any IO. Mismatches (including
    "the file vanished") abort the call with `Error::conflict`,
    `reason=stale_fingerprint`, `expected=<supplied>`, and the
    current `fingerprint=<token>` (or a `detail=<message>` entry
    when the fingerprint could not be read at all). The check sits
    *before* the truncate-mode temp-then-rename so a stale token
    cannot leave a half-written file behind.
  - `src/oran-tool/file_edit.cpp`: same schema addition plus the
    same pre-read guard, mirroring the write side. The downstream
    read still re-fingerprints internally for mid-read race
    detection; the new guard is the *intentional* freshness
    contract the agent asked for, not a duplicate.
  - `include/oran/tool/builtins.hpp` docstrings carry the new
    `expected_version` field for both tools.
  - `tests/tool/test_registry.cpp` adds 5 `[if_version]` cases:
    `file.write` happy path with matching token,
    `file.write` stale-token conflict (asserts the `reason`,
    `expected`, and `fingerprint` context entries plus that the
    file body is untouched), `file.write` missing-path conflict,
    `file.edit` happy path with matching token, and `file.edit`
    stale-token conflict. Two small helpers (the
    `read_and_write_rule_set` / `read_and_edit_rule_set` factories
    and `dispatch_read_and_extract_token`) keep the wiring boilerplate
    out of every case body.
  - `src/oran-bootstrap/bootstrap.cpp` version banner bumped to
    `2.0.0-slice46`.

### Design Intent

Putting the pre-mutation fingerprint check at the *tool* layer (not
the io layer) is intentional. `oran-io::write_text_file` /
`read_text_file` already have a mid-write/read race story; the
`expected_version` guard is a different contract — "the agent
asserted what the file looks like *before* this call" — and that
assertion is meaningless without the canonical-path canonicalisation
that lives in `tool::Workspace` and the version-token format that
lives in `oran-tool`. Pushing the check into `oran-io` would force
the io layer to grow a dependency on `oran-permission` (for the
SHA-256 path hash) and on the token wire format, both of which are
above its station.

The decision to surface "the file vanished" as `conflict` (rather
than letting `compute_file_fingerprint`'s `not_found` propagate
verbatim) keeps the error model consistent for the agent: any
mismatch between the supplied token and the on-disk state is a
*stale fingerprint*, not a separate `not_found`. The agent's recovery
path is the same regardless — drop the token, re-read, retry — so we
funnel every flavor through one error kind. The original
`compute_file_fingerprint` error message rides along in a `detail`
context entry so an operator looking at audit logs can still tell
the two situations apart.

Lifting the token helper into a private header (rather than a public
one under `include/oran/tool/`) keeps the wire format an
implementation detail. If a future slice needs to expose it (say a
`tool::token::format(canonical_path, fingerprint)` free function for
prompt rendering), the move from `src/` to `include/` is mechanical.
Promoting prematurely would lock us into a public-API churn risk
the v1 catalog does not need.

The schema-side `expected_version: string` is optional — agents that
do not care about staleness keep their existing call shape. That
keeps the change strictly additive: the slice-18/19 happy-path tests
still pass without modification.

### Files Modified

- `src/oran-tool/version_token.hpp` (new private header)
- `src/oran-tool/file_read.cpp` (consumes the lifted helper)
- `src/oran-tool/file_write.cpp` (schema + pre-mutation guard)
- `src/oran-tool/file_edit.cpp` (schema + pre-mutation guard)
- `include/oran/tool/builtins.hpp` (docstrings)
- `tests/tool/test_registry.cpp` (5 new `[if_version]` cases + helpers)
- `src/oran-bootstrap/bootstrap.cpp` (version banner)
- `docs/STATUS.md`
- `docs/ARCHITECTURE.md`
- `docs/QUALITY_SCORE.md`
- `docs/product-specs/0011-file-view-and-caching.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 46, new history pointer, refreshed
  `oran-tool` test counts (121 / 1022), and the next-intended-slice
  narrative now names the `BoundedCache` consumers (line-offset
  index, file-view cache, regex compile cache) as the v1.1 work
  on top of the now-complete v1.
- `docs/ARCHITECTURE.md` — slice-status block gains the slice-46
  callouts for `file.write` and `file.edit`; the `oran-tool` inventory
  rows for both tools now name `expected_version` in the input shape
  and the `Error::conflict reason=stale_fingerprint` exit.
- `docs/QUALITY_SCORE.md` — Test framework row refreshed (`oran-tool`
  121 / 1022).
- `docs/product-specs/0011-file-view-and-caching.md` — Status block
  gains a slice-46 entry; v1 is marked complete; v1.1 named as next.
- `docs/releases/feature-release-notes.md` — user-visible release
  note.

### Validation

- Commands run:
  - `xmake build oran-tool`
  - `xmake build test-tool` / `xmake run test-tool '[if_version]'`
  - `xmake test` (all 10 buckets pass)
- Tests added/changed:
  - `tests/tool/test_registry.cpp` adds 5 `[if_version]` cases
    covering both mutation tools. `tests/tool` reports 121 / 1022
    (was 116 / 980).
- Bench impact: not measured — the new pre-mutation guard adds one
  `compute_file_fingerprint` stat + one SHA-256 of the canonical path
  *only when* `expected_version` is supplied; the default-shape
  callsite is unchanged. The slice-18/19 bench numbers in
  `QUALITY_SCORE.md` still apply for the no-token call.
- Compile-budget delta: not measured. The new private header is a
  tiny inline template-free helper; the only new public-include cost
  is `<oran/io/fingerprint.hpp>` being pulled into
  `file_write.cpp` / `file_edit.cpp`, both of which already include
  the rest of the io surface.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none. v1.1 (line-offset index, file-view cache,
  regex compile cache, singleflight reads, external-edit awareness,
  output cap on `file.search`) is the next 0011 milestone and
  consumes the slice-44 `BoundedCache` primitive.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`
