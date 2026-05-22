# 0011 — File-View System: Range Reads, Change Detection, and Caching

## User Problem

An agent that edits code must trust its view of the workspace. Today's file
tools are correct in isolation but invent a different stale-file story per
tool:

- `file.read` returns the entire body and no version token. Agents have no
  way to say "edit only if the file is still the one I read."
- `file.edit` reads, computes, and writes without a fingerprint round-trip,
  so a concurrent process change between read and write silently overwrites
  the newer content (TOCTOU).
- `file.search` reopens each match candidate from scratch; even on a stable
  tree, a long agent turn re-pays the IO cost of cold reads.
- There is no shared notion of "this path's bytes" that `file.read`,
  `file.edit`, `file.search`, and the future `code.symbols` /
  `code.references` can agree on.

The risk is not "slow reads". The risk is *the agent confidently acting on a
stale or unauthorised view of the workspace*. The user pays for that with
overwritten work, with edits that apply to a different file than the agent
inspected, and with a confused recovery loop.

This spec defines the file-view contract: a versioned, range-aware,
audit-respecting view of workspace files that every read/mutate tool shares,
plus the cache/index scaffolding that makes repeated reads cheap *without*
making them wrong.

Today's seams that motivate this spec:

- `file.read` accepts only `{path}` and returns the entire body
  (`src/oran-tool/file_read.cpp`).
- `oran-io::ReadTextOptions` already has a `max_bytes` cap; neither the IO
  surface nor the tool schema exposes ranges, fingerprints, or change tokens.
- `tool::Output` is text-only (`include/oran/tool/registry.hpp:80-87`);
  precise read metadata cannot be returned cleanly until the `Output v2`
  shape lands (tracked in
  [`../exec-plans/tech-debt-tracker.md`](../exec-plans/tech-debt-tracker.md)).

## Scope (v1)

The first slice delivers the *correctness* layer of the file-view system.
Optimisations and caches come later — and only on top of this contract.

- **`io::ReadTextResult`** as the new return type of `io::read_text_file`:
  ```cpp
  struct ReadTextResult {
    std::string         text;
    FileFingerprint     fingerprint;     // size + mtime_ns + optional sha256
    std::uint64_t       start_line;      // 1-based; 1 when no range requested
    std::uint64_t       end_line;        // inclusive
    std::uintmax_t      returned_bytes;
    bool                truncated;       // true when output cap fired
  };
  ```
  The text-only overload stays as a thin wrapper for non-tool callers.
- **`io::FileFingerprint`** as a stable, byte-cheap identity:
  ```cpp
  struct FileFingerprint {
    std::uintmax_t          size_bytes;
    std::uint64_t           mtime_ns;
    std::optional<std::string> sha256;   // populated when compute_hash=true
  };
  ```
  Lifted into a single header so `oran-tool`, the future `oran-memory`
  index, and the future `code.*` tools all speak the same shape.
- **`io::FileRange`** input: mutually exclusive line range
  (`start_line` + `line_count`, 1-based) or byte range
  (`offset_bytes` + `length_bytes`). Validation:
  - both set → `invalid_argument`
  - any zero field → `invalid_argument`
  - byte range that ends inside a UTF-8 boundary → adjust to a valid
    boundary and report the adjusted range in `ReadTextResult`, or reject
    with `invalid_argument` (config knob: default *adjust*).
- **`io::ReadTextOptions` v2**:
  ```cpp
  struct ReadTextOptions {
    std::uintmax_t          max_bytes{16U * 1024U * 1024U};
    std::optional<FileRange> range;
    bool                    compute_hash{false};
  };
  ```
  Output cap (`max_bytes`) enforces *returned bytes*, never *file size*.
  A huge line range fails or truncates before flooding the prompt.
- **`file.read` v2** schema:
  ```json
  {
    "path": "...",
    "start_line": 120,
    "line_count": 80,
    "offset_bytes": null,
    "length_bytes": null,
    "max_bytes": 65536,
    "if_version": "<previous version token, optional>"
  }
  ```
  Returned text is wrapped in a small header line so today's text-only
  `tool::Output` can still carry it: `<path>:<start_line>-<end_line>
  fingerprint=<token> bytes=<n>[ truncated]`. When `Output v2` lands, the
  same payload moves into `Output::structured` verbatim.
- **Version token shape**: opaque string
  `v1:<sha256(canonical_path)>:<size>:<mtime_ns>` (sha256 of *path*, not
  content). Stable across tools; embeds the cheap fingerprint so the agent
  cannot accidentally feed a token from a different file into
  `file.edit(expected_version=...)`. The strong content hash lives in
  `FileFingerprint::sha256` and only fires when `compute_hash=true`.
- **`if_version` semantics**: when supplied and the current fingerprint
  matches, `file.read` returns `Error::not_modified` (new kind) instead of
  re-sending content. The agent's transcript carries the token; the prompt
  does not re-pay the bytes.
- **Mid-read change detection**: capture fingerprint before *and* after the
  blocking read. On size/mtime/inode change:
  - small files (< 64 KiB): retry once, then return `conflict`.
  - large or ranged reads: return `conflict` immediately so the caller can
    re-read with a fresh token.
- **Range-read implementation**:
  - Byte ranges use `seekg` and stream only the requested span. Never read
    the full file then slice.
  - Line ranges stream until `start_line`, then copy up to
    `line_count` / `max_bytes`. Avoid building a full `std::vector<line>`.
- **`file.edit` / `file.write` token contract**: `expected_version` field
  becomes optional in v1, *required* in `file.modify` (spec 0011 v1.1
  below). When supplied, mismatch fails as `Error::conflict` with
  `reason=stale_fingerprint` and the current token in context, forcing the
  agent to re-read.
- **`file.edit` / `file.write` size caps**: both mutation tools accept an
  optional `max_bytes` field (default and hard ceiling 16 MiB, matching
  `io::ReadTextOptions::max_bytes`). `file.write` refuses an oversized
  `content` payload before touching the target path. `file.edit` applies the
  same cap to the read and to the final replacement output, so an edit cannot
  create text that a follow-up `file.read` would reject.

## Scope (v1.1)

Once v1 has shipped, the optimisation layer becomes safe to build because
correctness is anchored:

- **`BoundedCache<Key,Value>`** generic primitive (also referenced by spec
  0012):
  ```cpp
  struct BoundedCache::Options {
    std::size_t            max_entries;
    std::size_t            max_bytes;     // optional, 0 = unlimited
    std::chrono::seconds   ttl;
  };
  ```
  LRU on access, TTL on age, byte budget on payload size. Eviction policy
  is explicit in the type name, never hidden in a comment.
- **Line-offset index** as the first cache: maps line number → byte offset
  for files larger than a configured threshold (default 256 KiB). Built
  lazily on first range read; invalidated on any `file.write` /
  `file.edit` / `file.modify` / `file.delete` for the same canonical path.
  Memory cost is `O(lines * 8 bytes)`, far cheaper than caching the body.
- **`file.search` re-uses the line-offset index** so that a follow-up
  range-read of a match doesn't rescan the file from the top.
- **`file.search` literal-pattern cache**: compiled re2 patterns held in a
  small `BoundedCache<(pattern, options), unique_ptr<re2::RE2>>` keyed per
  agent turn. Bounded — never an unbounded `unordered_map`.
- **`file.search` ignore predicate**: honour `.gitignore`, `.ignore`, and a
  built-in skip list (`.git`, `build`, `.xmake`, `node_modules`,
  `.orangutan/cache`). Predicate lives on `tool::Workspace` per spec 0013
  so `directory.scan` shares it.
- **Singleflight reads**: ten concurrent `file.read` calls for the same
  `(canonical_path, range)` share one filesystem read instead of stampeding
  the executor.
- **External-edit awareness**: when an `asio` filesystem watcher is
  available, register against `<workspace>/**`; on a watcher event, mark the
  affected canonical path stale. Without a watcher, validate metadata
  (`stat` only) before every cache hit; on uncertainty, miss.
- **Output cap on `file.search`**: in addition to `max_matches`, an output
  byte cap so a small match count of very long lines does not flood the
  prompt.

## Scope (v2)

- **`file.modify`** — structured multi-edit transaction:
  ```json
  {
    "path": "...",
    "expected_sha256": "...",
    "edits": [
      {"old": "...", "new": "..."},
      {"old": "...", "new": "..."}
    ]
  }
  ```
  Applies all edits against the original file and commits once. Reports
  conflicts per-edit so the agent repairs only the failed hunk. Replaces
  the current `file.edit` for heavy coding workloads; `file.edit` keeps its
  simple single-replacement contract for trivial fixes.
- **Persisted index store** under `<workspace>/.orangutan/cache/indexes/`
  for line-offset indexes that survive process restarts. Every persisted
  index carries a header: schema version, orangutan build/slice,
  workspace root canonical path, config hash, source fingerprint strategy.
  Bound on disk separately from entry count
  (`max_index_bytes_per_workspace`).
- **Text-search shard index** (per-file trigram / line-offset metadata)
  built only after a workspace crosses a size threshold (default 5 000
  files). Optional; `file.search` falls back to the un-indexed walk
  whenever the shard is absent or stale.
- **Vector-search backend** is deferred — keep it out of this spec; spec
  0005 (memory) owns retrieval strategy.

## Out Of Scope

- Cross-process file locking. The per-path lock table is the *agent
  runtime's* concurrency primitive (spec 0012 §Path-level lock table); two
  separate `orangutan` processes editing the same file rely on the
  filesystem's rename atomicity (spec is documented in
  [`../design-docs/io-runtime.md`](../design-docs/io-runtime.md) "Atomic
  Writes").
- Binary diff / patch tools (`file.binary_patch`). The agent is text-first;
  attachments handle binary payloads.
- Server-side hosted file views. Single-binary, local-first per
  [`../design-docs/agent-platform.md`](../design-docs/agent-platform.md)
  "Anti-Goals".

## Acceptance Criteria

1. **Range read correctness.** A line-range read of lines 120–199 from a
   10 MiB source file returns ≤ 80 lines, sets `start_line=120`,
   `end_line=199` (or fewer if `max_bytes` truncates), and the returned
   bytes count matches the wire payload exactly. Byte-range tests cover
   exact-boundary, UTF-8-mid-boundary (adjust mode), and overflow.
2. **Fingerprint stability.** Two consecutive `file.read` calls on an
   unchanged file produce identical version tokens. Touching the file
   (mtime bump, no content change) produces a different token; rewriting
   the file via `file.write` produces a different token.
3. **`if_version` short-circuit.** A `file.read` call with `if_version`
   matching the current fingerprint returns `Error::not_modified` with the
   *same* token in context, no body. A mismatching `if_version` returns
   fresh content and the new token. Tested for size change, mtime change,
   and content rewrite.
4. **Stale-edit detection.** `file.edit` (and later `file.modify`) with a
   stale `expected_version` returns `Error::conflict` with
   `reason=stale_fingerprint` and the *current* token in context; the file
   on disk is untouched. Tested against an external `touch` between read
   and edit, and against an external `truncate`.
5. **Mid-read race.** A read whose mtime/size/inode changes between the
   pre- and post-read fingerprints retries once for files < 64 KiB,
   returns `conflict` for larger or ranged reads. Pinned via a fault-injecting
   filesystem mock.
6. **`max_bytes` discipline.** A 1-line range whose single line is 4 MiB
   long fails with `truncated=true` when `max_bytes` is below the line
   size; the prompt never receives the over-cap bytes. Tested at boundary
   and boundary+1.
7. **Cache safety.** Every `file.read` cache hit still records an
   `AuditEvent` and publishes `hook::Event::tool_after`. A `file.write` /
   `file.edit` / `file.delete` invalidates affected cache + line-offset
   entries *synchronously* before returning success. Pinned via a test
   that interleaves cached read → write → cached read on the same path.
8. **Bounded growth.** `BoundedCache` rejects no inserts but evicts oldest
   on capacity, then refuses to cache items larger than its byte budget.
   Capacity, byte budget, and TTL are observable via a stats accessor for
   the future `oran-log` to publish.
9. **Output v2 forward compat.** v1's text-header rendering is feature-flag
   compatible with the future `Output::structured` field: the same
   `(path, start_line, end_line, fingerprint, bytes, truncated)` tuple
   round-trips through both shapes.
10. **`tests/io/`** and **`tests/tool/`** ≥ 90% coverage of the matrix
    (intent × range kind × fingerprint hit/miss × cache hit/miss ×
    invalidation source).

## Design Doc Cross-References

- [`../design-docs/io-runtime.md`](../design-docs/io-runtime.md) — owns the
  `oran-io` surface (`ReadTextResult`, `FileFingerprint`, `FileRange`);
  the "Future Slices" section gains the v1 shape when this spec ships.
- [`../design-docs/tool-runtime.md`](../design-docs/tool-runtime.md) —
  `tool::Output` v2 (the `{text, data, attachments, cost, is_error}` shape
  in the design doc) is a prerequisite for v1.1 onward; v1 lives inside
  the current text-only `Output` via the header-line rendering above.
- [`0013-workspace-and-path-policy.md`](0013-workspace-and-path-policy.md)
  — every path in the file-view system goes through `tool::Workspace`
  resolution first; the version token embeds the canonical path so two
  agents with different workspaces cannot collide.
- [`0012-tool-scheduler-and-state.md`](0012-tool-scheduler-and-state.md) —
  per-path read/write locks live in the scheduler; the file-view system
  consumes them rather than rolling its own.
- [`0005-memory-system.md`](0005-memory-system.md) — `FileFingerprint`
  becomes the citation primitive when memory ties recalled facts back to
  specific file revisions.

## Risks

- **`tool::Output` churn.** Returning richer metadata before `Output v2`
  ships means re-parsing the text header from agent code. Mitigation: ship
  v1 with the header-line shape and a stable `(path, start_line,
  end_line, fingerprint, bytes, truncated)` parser in `oran-tool`; flip to
  `Output::structured` in a one-line callsite change when v2 lands.
- **Fingerprint forgery.** A motivated adversary can produce a file with
  the same size + mtime as the original. Mitigation: high-trust paths
  (`file.modify` v2, memory citations) set `compute_hash=true` and use the
  SHA-256 token; low-trust paths (interactive `file.read`) skip the hash
  to stay cheap.
- **Cache memory growth.** A pathological agent calls `file.read` on every
  file in a 10 000-file repo. Mitigation: bounded LRU + byte budget +
  TTL; configurable via `runtime.file_view_cache.{max_entries,max_bytes,ttl_seconds}`.
- **Watcher portability.** `asio` filesystem watchers are non-uniform
  across platforms. Mitigation: v1.1 ships the watcher as an opt-in
  (`runtime.file_view_cache.watcher=auto|inotify|off`) with a lazy `stat`
  fallback that keeps correctness even when the watcher is disabled.
- **UTF-8 boundary policy ambiguity.** "Adjust to valid boundary" can
  surprise agents that compute byte offsets server-side. Mitigation:
  the response always echoes the *adjusted* range; a strict caller can
  set `runtime.file_view.utf8_byte_policy=reject` to force
  `invalid_argument` instead.

## Validation

```sh
xmake build oran-io oran-tool
xmake test test-io                    # ReadTextResult + FileRange + fingerprint suite
xmake test test-tool                  # file.read v2 + file.edit token + cache suite
xmake build bench-oran-io bench-oran-tool
xmake run bench-oran-io  read_range   # full vs. 100-line range from 10 MiB file
xmake run bench-oran-tool file_view_cache  # cold vs. hot, stat vs. SHA-256 validation
```

## Out-of-Band Cross-Cuts

- `docs/design-docs/io-runtime.md` "Public Surface" gains the v2 shapes;
  the slice that lands them moves the old `read_text_file` overload to
  "compat alias".
- `docs/design-docs/tool-runtime.md` "Tool Handler Shape" notes that
  `file.read` v2 is the first consumer of the `Output v2` migration plan.
- `docs/exec-plans/tech-debt-tracker.md` retires the deep-review §File
  read range / change detection / cache plan rows in the slice that
  closes v1.
- `docs/SECURITY.md` "File tools" cross-links to v1's audit-on-cache-hit
  invariant so the security promise stays unambiguous.
