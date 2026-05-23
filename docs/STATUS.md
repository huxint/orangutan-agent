# Current State

> **One-screen project snapshot.** Read this *first* in every new session to
> orient on where the project is right now. Update in the same commit as
> the history entry that moves it — see
> [`rules/docs-in-sync.md`](rules/docs-in-sync.md).

## Snapshot

- **Slice:** 68 (`xmake run orangutan` reports slice 68)
- **Last completed history:**
  [`histories/2026-05/20260524-0645-tool-search-registry-lookup.md`](histories/2026-05/20260524-0645-tool-search-registry-lookup.md)
- **Active exec-plan:** none — current slice intent fits inside the
  `Next intended slice` bullet below; see
  [`PLANS_GUIDE.md`](PLANS_GUIDE.md) "When NOT To Create A Plan".
  When `active/` is non-empty, link the file path here instead.
- **Next intended slice:** Continue along the spec dependency graph
  (0013 → 0011 + 0012 → 0014 → 0016 → 0017 → 0015 → 0018). Slice 68
  lands spec 0016's registry-owned deferred-tool lookup primitive:
  `register_builtins` now includes the non-deferred `tool.search`
  built-in, categorized as `runtime`, with no required runtime
  capability. The handler searches the current `Registry::catalog()`
  snapshot by exact `name`, exact `category`, and/or declared
  `capability`; `Registry::dispatch` sets and restores the non-owning
  `DispatchContext::registry` pointer so the handler can inspect the
  live dispatching registry without capturing a self-reference inside a
  movable registry value. At least one selector is required and supplied
  selectors are ANDed. Successful calls return a text fallback plus
  structured `Output::data_json` shaped as
  `{kind:"tool_search", query, match_count, matches[]}`, where each
  match carries `name`, `description`, nested `input_schema`,
  `required_capabilities`, `deferred`, and nullable `category`;
  `Output::usage.match_count` mirrors the number of matches. This is
  deliberately not the per-session promotion side effect yet: there is
  no `oran-agent::SessionState` or prompt builder to own LRU/TTL
  promotions, so the remaining 0016 work is active-tool config,
  prompt-builder integration, and session promotion before/alongside
  the first 0017 fake-provider agent loop tracer bullet. Slice 67
  closes spec 0014's audit usage fan-out for the pre-scheduler direct
  dispatch path: `permission::AuditSink` now exposes
  `update_metadata(AuditMetadataUpdate)`, `RecordingAuditSink` and
  `StorageAuditSink` implement it, and `storage::AuditRepository` can
  replace the newest matching `audit_events.metadata_json` value without
  appending a second permission-decision row. `Registry::dispatch` still
  records the permission decision before any handler side effects; when an
  allow or ask-approved handler returns a successful `tool::Output`,
  dispatch applies output caps, serializes non-empty `Output::usage` under
  `metadata_json.usage`, and best-effort enriches the same audit row. The
  direct-dispatch enrichment covers the shipped filesystem built-ins and
  the cap flags from slice 66. Provider adapter mapping remains the
  remaining spec-0014 item; scheduler ownership of cap options and any
  stronger per-batch audit correlation belong to the upcoming spec-0012 /
  agent-loop work. Slice 66
  closes spec 0014's byte-cap item for the pre-scheduler dispatch
  boundary: `<oran/tool/output.hpp>` now exposes
  `OutputCapOptions`, `OutputCapReport`, and `apply_output_caps`, and
  `Registry::dispatch` applies `DispatchContext::output_caps` to
  successful handler output before returning it or publishing
  `tool_after`. Text overflow is truncated on a UTF-8 code-point
  boundary and sets `usage.truncated`; structured-data overflow drops
  only `data_json` and sets `usage.data_dropped`, leaving the text
  fallback intact. `oran-config` now parses the documented
  `runtime.tool_output.max_text_bytes` / `max_data_bytes` block
  (defaults 256 KiB / 1 MiB) so the future scheduler/agent owner can
  thread operator caps into `DispatchContext` instead of hard-coding
  them. Provider adapter mapping remained downstream, and slice 67 adds
  audit usage metadata enrichment. Slice 65
  closes spec 0014's hook raw-data redaction item: `hook::Sink` now
  exposes `kind()` with `SinkKind::default_` and
  `SinkKind::trusted_local`, `hook::InProcessSink` stores the chosen
  kind, `ToolAfterPayload` can carry optional raw structured
  `data_json`, `Registry::dispatch` copies successful
  `Output::data_json` into the hook payload, and
  `Bus::publish_advisory` clears that field for every sink that is not
  `trusted_local`. Default sinks therefore keep text + usage only, while
  trusted-local observers can receive the raw structured bytes. With
  `file.read` (slice 62), `file.search` (slice 63), and
  `directory.list` (slice 64) migrated and the mutation tools holding
  measured usage counters from slice 61, the built-in side of spec 0014's
  structured-output migration is done. Provider adapter mapping,
  byte-cap enforcement, and audit usage fan-out were downstream at that
  point; later slices shipped byte caps and same-row audit usage metadata
  enrichment.
  Slice 64
  continues spec 0014's built-in structured-output migration for
  `directory.list`: the handler keeps the existing
  `<path>:<kind>:<size_bytes or '-'>` text rendering, now fills
  `Output::data_json` with a `{kind:"directory_list", path,
  include_hidden, max_entries, entry_count, entries[]}` payload
  (each entry carries `{name, path, kind, size_bytes}` with JSON null
  for non-regular kinds), and fills `Output::usage.files_touched=1`
  plus `match_count=entry_count` so audit fan-out can see directory-walk
  cost without parsing prose.
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
  successful handler returns. Provider-adapter mapping remains downstream;
  later slices migrated `file.search` / `directory.list` structured
  `data_json`, shipped trusted-local hook raw-data redaction, added the
  slice-66 dispatch-boundary output-cap helper, and added slice-67
  same-row audit usage metadata enrichment. Slice 59
  starts the prompt-catalog cache prework shared by specs 0012 and
  0016: `core::ToolDef` now carries the documented `deferred` and
  `category` metadata, and `oran-tool` exposes `tool::CatalogRenderer`,
  a single-strand deterministic renderer that sorts catalog snapshots by
  tool name, renders non-deferred tools as canonical schema blocks,
  renders deferred tools as name/description index rows, and memoises
  full-schema blocks in a bounded 256-entry cache keyed by stable
  rendered-block fields plus renderer version. The public stats report
  aggregate cache counters only. This is not yet the `oran-prompt`
  builder, active-tool config, or promotion-set slice; slice 68 adds the
  registry-local `tool.search` lookup primitive that this renderer's
  future prompt builder will advertise as an active tool.
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
- `oran-storage`: 61 cases / 718 assertions.
- `oran-config`: 26 cases / 184 assertions.
- `oran-permission`: 88 cases / 403 assertions.
- `oran-hook`: 17 cases / 109 assertions.
- `oran-tool`: 166 cases / 1588 assertions.
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
