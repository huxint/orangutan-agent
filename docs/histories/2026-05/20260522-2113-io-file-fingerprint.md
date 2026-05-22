## [2026-05-22 21:13] | Task: io::FileFingerprint + compute helper (spec 0011 v1 foundation)

### Execution Context

- Agent: `Claude Code`
- Base model: `Opus 4.7`
- Runtime: `Claude Code CLI`
- Linked plan: none — scoped first step of spec 0011

### User Query

> Deeply understand the project architecture and current implementation
> progress, continue advancing the project code, two slices, one commit per
> slice; ultrathink.

### Changes Overview

- Areas: `oran-io` (public surface + impl + tests + bench), spec 0011 status.
- Key actions: introduced `io::FileFingerprint` (`size_bytes`,
  `mtime_ns`, reserved `optional<string> sha256`) in a new public header
  `<oran/io/fingerprint.hpp>`, exposed it via `<oran/io.hpp>`, and shipped
  a synchronous `io::compute_file_fingerprint(path)` helper that stats the
  file once and converts the platform `file_time_type` to UTC nanoseconds
  via `clock_cast`. Errors are precise: empty path → `invalid_argument`,
  missing file → `not_found`, non-regular file → `io`. The helper tolerates
  libstdc++'s ENOENT-with-`status()` quirk so callers reliably get
  `Error::not_found` for the missing-file case. Added seven Catch2 cases
  covering size + mtime correctness, stability under repeated reads,
  mtime-change visibility, size-change visibility, missing file rejection,
  directory rejection, and empty-path rejection. Added an A/B nanobench
  scenario (`io.direct_stat_pair` vs `io.compute_file_fingerprint`) so the
  wrapper's overhead is measurable and pinned. Bumped the version banner
  to slice 42.

### Design Intent

Spec 0011 v1 names a `FileFingerprint { size_bytes, mtime_ns, sha256 }`
shape that the future range-aware `read_text_file` will return inside a
`ReadTextResult`, and that the agent loop will use as the `if_version`
token. The shape is small and orthogonal to anything else in `oran-io`,
so a one-slice introduction (data type + helper + tests + bench) is the
cheapest way to make follow-up slices land cleanly. SHA-256 hashing
deliberately stays out of this slice — wiring it requires either pulling
libsodium into `oran-io` or adding a new core hash helper, and the spec
itself flags content hashing as opt-in via a future
`ComputeFingerprintOptions::compute_hash` flag.

The helper is synchronous because the underlying syscalls are pure metadata
ops; making it `Awaitable<...>` would add executor plumbing for no real
work. Async callers can `co_await async::post(executor, []{ return
io::compute_file_fingerprint(path); })` if they need to schedule the stat
off the calling strand.

### Files Modified

- `include/oran/io.hpp`
- `include/oran/io/fingerprint.hpp` (new)
- `src/oran-io/fingerprint.cpp` (new)
- `tests/io/test_fingerprint.cpp` (new)
- `bench/io/main.cpp`
- `bench/io/scenarios/fingerprint.cpp` (new)
- `src/oran-bootstrap/bootstrap.cpp` (slice banner bump)

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 42, new history pointer, refreshed `oran-io`
  test count (23/90), and the next-intended-slice narrative.
- `docs/ARCHITECTURE.md` — `oran-io` library inventory row mentions
  fingerprint surface; the slice-status block records slice 42.
- `docs/design-docs/io-runtime.md` — "Future Slices" range-reads bullet
  now points at the shipped `FileFingerprint` foundation.
- `docs/product-specs/0011-file-view-and-caching.md` — Scope (v1) head
  records slice 42 status; the remaining v1 bullets keep their shape.
- `docs/QUALITY_SCORE.md` — IO row + test counts refreshed.
- `docs/releases/feature-release-notes.md` — user-visible release note.

### Validation

- Commands run:
  - `xmake build oran-io`
  - `xmake build test-io` / `xmake run test-io`
  - `xmake build bench-io` / `xmake run bench-io`
  - `xmake test` (all 10 buckets pass)
  - `scripts/ci.sh`
- Tests added/changed:
  - `tests/io/test_fingerprint.cpp` adds seven cases. `tests/io` now
    reports 23 cases / 90 assertions.
- Bench impact: `io.direct_stat_pair` ~884 ns vs.
  `io.compute_file_fingerprint` ~1,312 ns — the helper adds ~430 ns
  over the raw stat pair (existence + regular-file check + nanosecond
  conversion + `Error::not_found` plumbing). This is the baseline the
  future content-hash path will be measured against.
- Compile-budget delta: not measured. Incremental rebuild of the new TUs
  linked in under a couple seconds on this environment; this is not the
  reference hardware gate.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none. Future slice introduces `ReadTextResult`,
  `FileRange`, and the range-aware `read_text_file` overload; another
  future slice wires `compute_hash=true` once we settle whether
  libsodium-`crypto_hash_sha256` lives in `oran-io` directly or behind a
  new `oran-core` digest helper.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`
