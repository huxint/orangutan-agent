# Current State

> **One-screen project snapshot.** Read this *first* in every new session to
> orient on where the project is right now. Update in the same commit as
> the history entry that moves it — see
> [`rules/docs-in-sync.md`](rules/docs-in-sync.md).

## Snapshot

- **Slice:** 64 (`xmake run orangutan` reports slice 64)
- **Last completed history:**
  [`histories/2026-05/20260524-0540-tool-directory-list-structured-output.md`](histories/2026-05/20260524-0540-tool-directory-list-structured-output.md)
- **Active exec-plan:** none — current slice intent fits inside the
  `Next intended slice` bullet below; see
  [`PLANS_GUIDE.md`](PLANS_GUIDE.md) "When NOT To Create A Plan".
  When `active/` is non-empty, link the file path here instead.
- **Next intended slice:** Continue along the spec dependency graph
  (0013 → 0011 + 0012 → 0014 → 0016 → 0017 → 0015 → 0018). Slice 64
  continues spec 0014's built-in structured-output migration for
  `directory.list`: the handler keeps the existing
  `<path>:<kind>:<size_bytes or '-'>` text rendering, now fills
  `Output::data_json` with a `{kind:"directory_list", path,
  include_hidden, max_entries, entry_count, entries[]}` payload
  (each entry carries `{name, path, kind, size_bytes}` with JSON null
  for non-regular kinds), and fills `Output::usage.files_touched=1`
  plus `match_count=entry_count` so audit fan-out can see directory-walk
  cost without parsing prose. With `file.read` (slice 62),
  `file.search` (slice 63), and `directory.list` (slice 64) migrated
  and the mutation tools holding measured usage counters from slice 61,
  the built-in side of spec 0014's structured-output migration is done.
  Provider adapter mapping, scheduler byte caps, audit usage fan-out,
  and hook raw-data redaction remain downstream spec-0014 items.
  Slice 63 migrated `file.search`: the handler keeps the existing
  `path:line:text` text rendering (with the slice-47 byte-cap and
  slice-20 match-cap trailing summary), now fills `Output::data_json`
  with a `{kind:"file_search", path, pattern, regex, matches[],
  match_count, truncated, truncation_reason, files_scanned,
  bytes_read}` payload, and fills `Output::usage.bytes_read`
  (cumulative scanned file bytes), `files_touched` (non-binary scanned
  file count), `match_count` (post-truncation match count), and the
  `truncated` cap flag. Slice 62
  continues spec 0014's built-in structured-output migration for
  `file.read`: the tool keeps the spec-0011 text header/body fallback,
  now fills `Output::data_json` with a JSON object carrying `kind`,
  `path`, requested `text`, `fingerprint`, `start_line`, `end_line`,
  `returned_bytes`, and `truncated`, and fills `Output::usage.bytes_read`,
  `files_touched`, and `truncated`. Slice 61 moved the current mutation
  tools onto usage counters: `file.write` fills
  `Output::usage.bytes_written` and `files_touched`; `file.edit` fills
  `bytes_read`, `bytes_written`, `files_touched`, and `match_count`; and
  `file.delete` fills `bytes_written=0` plus `files_touched=1`. The
  mutation tools keep their existing text summaries and leave `data_json`
  empty for the v1 migration path. Slice 60
  closed the deep-review "tool output is too small" finding and started
  spec 0014 inside `oran-tool`: `tool::Output` now lives in
  `<oran/tool/output.hpp>` with required `text`, optional serialized
  `data_json`, attachment metadata, usage counters, and `is_error`;
  `Output::text_only` preserves the v1-compatible text path, and
  `Output::error` can carry serialized structured error data. The public
  header stays `nlohmann`-free by storing structured payload bytes as a
  string for provider adapters to parse/serialize later. `Registry::dispatch`
  now copies `Output::usage` into `hook::ToolAfterPayload::usage` on
  successful handler returns. This is not yet provider-adapter mapping,
  scheduler byte-cap enforcement, audit usage fan-out, hook raw-data
  redaction, or structured `data_json` migration for `file.search` /
  `directory.list`. Slice 59
  starts the prompt-catalog cache prework shared by specs 0012 and
  0016: `core::ToolDef` now carries the documented `deferred` and
  `category` metadata, and `oran-tool` exposes `tool::CatalogRenderer`,
  a single-strand deterministic renderer that sorts catalog snapshots by
  tool name, renders non-deferred tools as canonical schema blocks,
  renders deferred tools as name/description index rows, and memoises
  full-schema blocks in a bounded 256-entry cache keyed by stable
  rendered-block fields plus renderer version. The public stats report
  aggregate cache counters only. This is not yet the `oran-prompt`
  builder, active-tool config, `tool.search`, or promotion-set slice.
  Slice 58
  closes spec 0011 v1.1's IO-layer watcher item: `oran-io` now exposes
  `watch_read_text_file_ranged_cache(executor, root, options)`, a
  cancel-aware Linux/inotify watcher that registers one directory or a
  recursive tree, drains filesystem events through an asio descriptor,
  and calls `invalidate_read_text_file_ranged_cache(path)` so external
  edits evict the affected file-view and line-offset-index entries
  without exposing cache keys. The returned `ReadTextFileWatchStats`
  reports only aggregate directories/events/invalidations. The watcher
  is not yet automatically started by bootstrap/config; that wiring waits
  for the runtime service that will own long-lived background tasks.
  Slice 57 landed the path-stale invalidation seam this watcher consumes:
  `core::BoundedCache` now has `erase_if(predicate)` for explicit
  non-policy invalidation, and successful `io::write_text_file` and
  `io::delete_file` reuse the same seam instead of clearing unrelated
  read-cache entries.
  Slice 56 closes spec 0012's approval-grant bounded-state item inside
  `oran-permission`: `ApprovalBroker::approve` now lazily reaps expired
  grants and keeps at most
  `ApprovalBroker::max_grants_per_identity` (64) live grant entries per
  identity, evicting the oldest same-identity grant when a new distinct
  `(tool, identity, input_hash)` triple would exceed the ceiling. Evicted
  tokens still verify cryptographically, but `ApprovalBroker::check`
  returns `reason=no_grant`. Slice 55
  closes spec 0013's v1 structural path-policy work inside `oran-tool`:
  `Registry::dispatch` now pre-resolves known filesystem built-in `path`
  inputs through `tool::Workspace` before permission evaluation, carries
  the absolute path to handlers via `DispatchContext::resolved_path`, and
  writes redacted resolver metadata (`input_path_hash`,
  `resolved_relative_path`, `workspace_root_hash`, symlink / parent /
  override flags, and resolver error kind/reason) under the existing
  `permission::AuditEvent::metadata_json` column. Path-policy failures are
  audited with the permission verdict but return before handlers run and
  before ask-approval replay is spent. Slice 54
  completed the public bounded-state observability surface for
  `oran-io`'s range-read caches: `read_text_file_ranged_cache_stats()`
  snapshots the private line-offset index and file-view cache
  `core::BoundedCache` counters (hits, misses, LRU/TTL/byte evictions,
  oversize rejections, current entries, current bytes) without exposing
  cache keys or paths. Slice 53's
  `read_text_file_ranged_singleflight_stats()` remains the paired
  in-flight-table snapshot.
  Spec 0013's remaining work is no longer v1 confinement plumbing; it is
  the v1.1 shared ignore predicate / display-helper work that waits for
  a second recursive consumer such as `directory.scan`, plus the future
  capability-gated `tool::Runtime::workspace()` accessor when
  `tool::Runtime` lands. The first
  provider adapter (Anthropic Messages) remains a multi-slice
  effort that needs an exec plan plus `oran-http` + libcurl wiring
  first; blocking hook semantics with veto are still gated on
  `oran-agent`; and wiring `check-compile-budget.sh` into
  `scripts/ci.sh` remains gated by the slice-28 reference-hardware
  precondition. The current `file.delete` and `directory.list`
  shapes are expected to be re-shaped in a later refactor: one
  unified delete tool covering both files and folders, and a
  recursive whole-project list (not just single-level children).
  Future built-in slices should not double down on per-kind splits
  like `directory.remove` or single-level enumeration.

## Library Health

Lifted from [`QUALITY_SCORE.md`](QUALITY_SCORE.md). `STATUS.md` summarizes;
`QUALITY_SCORE.md` explains.

| Score | Areas |
| ----- | ----- |
| **A** | *(none yet — pre-v1)* |
| **B** | Architecture docs, Build system, Async model, Security defaults, Supply chain |
| **C** | Compile-time discipline, Tests, Benches, IO, Storage, Config, Bootstrap, Provider system, Tool registry, Memory tiers, Permissions, Hooks, Channels, Orchestration, Automation, Web UI, CLI, Static analysis |
| **D** | Skills, Observability |

## Latest Library Surfaces

- `oran-core`: 69 cases / 450 assertions.
- `oran-async`: 9 cases / 43 assertions.
- `oran-io`: 49 cases / 286 assertions.
- `oran-storage`: 60 cases / 706 assertions.
- `oran-config`: 24 cases / 171 assertions.
- `oran-permission`: 86 cases / 390 assertions.
- `oran-hook`: 15 cases / 97 assertions.
- `oran-tool`: 156 cases / 1470 assertions.
- `oran-cli`: 5 cases / 30 assertions.
- `oran-bootstrap`: 48 cases / 153 assertions.

## Open Tech-Debt Rows

Lifted from [`exec-plans/tech-debt-tracker.md`](exec-plans/tech-debt-tracker.md).
Closed entries do *not* live here — the tracker is canonical.

- 2026-05-21 — Deep-review backlog: the stale root review artifact was
  deleted after its actionable findings were absorbed into the tracker and
  specs 0011-0018. Slices 31-36 closed the rank-0 items plus the P0
  follow-ups, and slice 60 closed the P2 `tool::Output` envelope item;
  remaining follow-ups are grouped P1/P2/P3 in the tracker.
- 2026-05-20 — `scripts/check-compile-budget.sh` exists and works (slice 28)
  but is not wired into `scripts/ci.sh`. Gated on CI provisioning xmake on
  the documented reference hardware (8-core / NVMe / native Linux);
  otherwise the gate fires on environmental drift, not real regressions.
- 2026-05-18 — Hook bus dispatch is advisory-only (slice 22); blocking
  semantics with veto for `tool_before` / `memory_*_before` /
  `permission_ask_rendered` are deferred to a follow-up slice.
- 2026-05-17 — `file.search` does not yet ship ripgrep-class optimisations
  (mmap, extension-based binary skip, multi-threaded walk).
  Adequate at slice 20 (~27 µs / 4-file tree) but 3-10× slower than a tuned
  scanner on repo-scale inputs. Re-bench once `oran-agent` produces a real
  workload measurement.
- 2026-05-17 — `scripts/check-prompt-preamble` static grep promised in
  `rules/prompt-design.md` not yet implemented (waits on first stable
  preamble template in `oran-agent`).
- 2026-05-17 — `bench/oran-agent/prompt_cache_hit_rate.cpp` regression
  scenario promised in `rules/prompt-design.md` not yet implemented
  (waits on `oran-agent` slice 1).
- 2026-05-14 — Generated `docs/generated/config.schema.json` not yet
  implemented.
- 2026-05-14 — bench A-vs-B scenarios listed in
  `bench/<lib>/README.md` are placeholders.
- 2026-05-14 — Frontend stack choice (Preact vs. plain JS) not yet
  decided.

## How To Update

1. The slice that lands a behavior change writes its history file.
2. The **same commit** updates this file: bump `Slice`, point
   `Last completed history` at the new file, refresh
   `Active exec-plan` (path or `none` + reason — see
   [`PLANS_GUIDE.md`](PLANS_GUIDE.md) "When NOT To Create A Plan"),
   refresh the test/assertion counts in "Latest Library Surfaces",
   and re-sync the tech-debt list from
   `exec-plans/tech-debt-tracker.md`.
3. `scripts/check-status-fresh.sh` fails the build if `STATUS.md`'s
   `Last completed history` pointer is older than the newest file under
   `docs/histories/`.

## See Also

- [`QUALITY_SCORE.md`](QUALITY_SCORE.md) — the per-area scoring rubric.
- [`releases/feature-release-notes.md`](releases/feature-release-notes.md)
  — chronological user-visible change log.
- [`exec-plans/tech-debt-tracker.md`](exec-plans/tech-debt-tracker.md)
  — open debt rows.
