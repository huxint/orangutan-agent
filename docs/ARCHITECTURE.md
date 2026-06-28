# Orangutan v2 — Architecture

This document is the **top-level map** of the rewrite. It defines the target state, not
the current state. Every block here is a place where a future execution plan or product
spec can be slotted in.

> The previous architecture is captured in
> [`../references/orangutan-legacy-audit.md`](references/orangutan-legacy-audit.md).
> Read it before touching subsystems — the friction list there is what this design exists
> to address.

## Mental Model

Orangutan v2 is **a single binary** that hosts **N agent runtimes** behind **M interfaces**
(CLI, desktop, channels, automation), backed by **shared storage and policy**. Everything is
asynchronous on a single executor; everything that crosses a process boundary goes through
a transport trait; everything observable goes through a hook surface.

```
                           ┌─────────────────────────────────────┐
  CLI REPL ────────────▶   │                                     │
  CLI single-shot ────▶    │       INTERFACE LAYER               │
  Desktop (Slint) ─────▶   │  cli • desktop • channel • cron     │
  Channels (QQ/…) ─────▶   │                                     │
  Automation (cron) ───▶   └─────────────────┬───────────────────┘
                                             │
                           ┌─────────────────▼───────────────────┐
                           │       AGENT RUNTIME LAYER           │
                           │                                     │
                           │   ReAct loop  ◀───▶  Tool registry  │
                           │      │                  │           │
                           │      ▼                  ▼           │
                           │   Provider system   Permissions     │
                           │      │                  │           │
                           │      ▼                  ▼           │
                           │   Memory tiers      Hook bus        │
                           │      │                  │           │
                           │      └────────┬─────────┘           │
                           │               │                     │
                           │     Orchestration (teams)           │
                           └───────────────┬─────────────────────┘
                                           │
                           ┌───────────────▼─────────────────────┐
                           │       PLATFORM LAYER                │
                           │  storage  •  config  •  secrets     │
                           │  http/ws  •  process  •  io          │
                           │  asio executor  •  logging          │
                           └─────────────────────────────────────┘
```

Each layer has **one direction of dependency**: interfaces depend on agent-runtime,
agent-runtime depends on platform. The platform layer never reaches up.

## Intended Repository Shape

```
harness-engineering/
├── include/oran/<lib>/...   public headers (forward-decl-heavy)
├── src/<lib>/...            implementation (heavy includes confined here)
├── tests/<lib>/...          one Catch2 bucket per library
├── bench/<lib>/...          one nanobench bucket per library
├── skeleton/                copy-paste starter for first xmake build
└── docs/                    system of record
```

The C++ libraries are listed below. **Each library is its own xmake target**, has its
own test bucket, its own bench bucket, and its own public header set under
`include/oran/<lib>/`.

## Library Inventory

> **Slice status (2026-05-25):** `oran-core` (now with `Error`/`Result`, the
> `Time` value type and ISO-8601 UTC helpers, the conversation types
> `Role`, `StopReason`, `Content` variant, and `Message`, the
> `ToolDef` declaration type (with `required_capabilities`,
> `deferred`, and `category`), the
> `core::str` RFC-3629 UTF-8
> helpers, the `Capability` vocabulary that ties tools to
> permission rules, and (slice 44) the generic
> `core::BoundedCache<Key, Value>` LRU + TTL + byte-budget primitive
> (slice 57 adds explicit `erase_if` invalidation), plus slice 79's
> shared `core::TurnId` trace/audit correlation value shape,
> `oran-async`,
> the file/directory MVP of `oran-io` plus slice 42's
> `io::FileFingerprint` + `compute_file_fingerprint` and slice 43's
> range-aware `io::read_text_file_ranged` returning
> `io::ReadTextResult { text, fingerprint, start_line, end_line,
> returned_bytes, truncated }` with `io::FileRange { LineSpan |
> ByteSpan }` mutual-exclusion validation, mid-read fingerprint
> capture (size/mtime drift -> retry once for whole-file reads
> under 64 KiB, `Error::conflict` for larger or ranged reads), and
> dual-end UTF-8 code-point boundary alignment for byte ranges, plus
> slice 50's bounded line-offset index for large-file line ranges
> (`core::BoundedCache`, 32 entries / 8 MiB / 10-minute TTL,
> invalidated for the affected canonical path after successful
> in-process writes/deletes), slice 52's bounded file-view cache for
> successful `ReadTextResult` payloads
> (`core::BoundedCache`, 64 entries / 16 MiB / 10-minute TTL, keyed by
> canonical path + range + max-bytes budget + cheap fingerprint,
> revalidated with `stat` before hits and invalidated for the affected
> canonical path after successful in-process writes/deletes), slice
> 57's public `invalidate_read_text_file_ranged_cache(path)` seam and
> slice 58's Linux/inotify-backed
> `watch_read_text_file_ranged_cache(executor, root, options)` event source
> that calls it for external edits, slice 54's public
> `ReadTextFileCacheStats` snapshot for the line-offset index and
> file-view cache, and slice 53's bounded singleflight table
> for concurrent cold `read_text_file_ranged` calls with observable
> `ReadTextSingleflightStats`, plus slice 153's
> `WriteTextDurability` fsync policy and PID+random exclusively-created
> temp leaves for atomic writes, and slice 154's public
> `io::run_blocking(executor, fn)` utility for short cancel-aware
> blocking calls that return `core::Result<T>`,
> the expected-only SQLite core +
> migration runner + SQL-file migration loader + async writer/reader `Pool`
> with per-slot statement caches + standalone per-connection `StatementCache`
> + `SessionRepository` (typed `core::Role` boundary) +
> `AuditRepository` + slice-78 `TraceRepository` of `oran-storage` (slice
> 79 aliases `storage::TraceId` to `core::TurnId` and adds nullable
> `audit_events.parent_turn_id` for trace/audit joins, slice 80's
> terminal-success loop writer for `trace_turns`, slice 82's explicit
> trace-disabled loop policy, slice 83's provider/tool cancellation trace rows,
> slice 84's provider/loop-boundary error trace rows, slice 85's generated
> trace turn ids, and slice 86's iteration-cap error trace rows),
> the `oran-config` JSON loader with `runtime`/`profiles` (including
> optional `protocol`)/`routes`/`session`/`desktop`/`trace`/`hooks.timeout_ms` plus the
> `permissions` and `agents.<name>.permissions` typed surfaces
> (layer-2/3 data of the three-layer rule merge),
> the foundation `RuleSet` + `Decision` of `oran-permission`
> with capability-aware gating (`Rule::capability`,
> `core::Capability`), the `Defaults::for_mode` baseline factory,
> and the three-layer `materialize` merge that concatenates
> defaults + global config + per-agent overlay, plus the slice-56
> `ApprovalBroker` cap of 64 live non-expired grants per identity
> (oldest same-identity grant evicted before inserting a new distinct
> triple),
> the config-loading + `RuntimeAssembly` slice of `oran-bootstrap`
> (a value-type bundle holding a fresh `permission::ApprovalBroker`
> and the active `permission::AuditSink` — `StorageAuditSink`
> over an internal `Pool` + `AuditRepository` when audit is on,
> `NullAuditSink` otherwise; audit defaults to enabled now that
> `oran-storage` ships its migrations compile-time-embedded via
> `#embed`; slice 87 also threads `config.trace().enabled` into the
> bundle by constructing a `storage::TraceRepository` over the same
> audit `Pool` when both audit and trace are enabled, exposing it via
> `RuntimeAssembly::trace_repository()` so the upcoming agent loop can
> persist spec-0018 rows without owning a second DB handle; slice 92
> adds the assembly-owned `hook::Bus` configured from
> `config.hooks.timeout_ms`, exposing it via `RuntimeAssembly::hook_bus()`
> for later agent/tool contexts; slice 99 preflights the configured default
> provider route through `provider::resolve_route` during regular startup,
> reports the resolved primary/fallback summary, and fails fast on route/profile
> config errors before CLI handoff while still avoiding credentials, adapters,
> network traffic, and `agent::Loop`; slice 100 adds an adapter-neutral
> `cli::PromptRunner` / `cli::run_async` seam so bootstrap can hand parsed
> prompts to a bootstrap-owned agent-loop runner without making `oran-cli` depend on
> `oran-agent`; slice 101 adds bootstrap's
> `AgentPromptRunner`, which borrows a supplied provider backend, wraps it in
> `provider::execution::Runtime`, binds `cli::OperatorPromptSink`, registers the
> builtin tool catalog, materializes config permissions, threads
> `RuntimeAssembly` workspace/audit/broker/hook/trace services into
> `agent::Loop`, and retains transcript state across prompts; slice 111
> adds the explicit `HttpProviderBackend` construction seam that resolves
> credentials, adapts `oran-http::Client` to `provider::ProtocolTransport`,
> registers the built-in Anthropic/OpenAI protocol factories, and returns a
> profile-routed provider system plus route for runner owners; slice 112
> switches configured-route `bootstrap::run` to that backend plus
> `cli::run_async`, while built-in empty defaults stay on the no-runner shell;
> slice 16
> adds `--mode` / `--agent` selectors to
> `--explain-rules` via the public `parse_explain_rules_selector`
> and `materialize_rules` helpers), the first `oran-cli` handoff shell plus
> the slice-95 terminal `OperatorPromptSink` for blocking
> `permission_ask_rendered` approvals, the slice-73 + slice-74 + slice-97 +
> slice-98
> `oran-provider` surface
> (slice-73 domain/cache-hint values — `Request`, `Response`, `Usage`,
> `RetryPolicy`, `PromptCacheHints`, `PromptCacheOptions`, and
> `make_prompt_cache_hints` over `prompt::RenderedPrompt`; slice-74 adds the
> abstract `provider::System` with `send(Request, Route, EventSink*) const`,
> the `provider::EventSink` streaming observer with default no-op
> callbacks, the `ProtocolKind` / `ModelTarget` / `Route` value shapes the
> loop will resolve once per turn, and the first concrete `provider::FakeProvider`
> with `ScriptedTurn` / `StreamDelta`, plan-exhaustion guard, cancel-aware
> scripted latency through `async::sleep_for`, and a delta-to-`Response`
> assembler that fans the same calls out to the observer; slice 97 adds
> `provider::execution::Runtime`, a `provider::System` decorator that consumes
> `Request::retry`, retries retryable errors per target, tries
> `Route::fallbacks` after primary exhaustion, observes cancellation during
> backoff through `async::sleep_for`, suppresses retry/fallback once visible
> stream output has been emitted, and fills missing `Response::model_used` with
> the selected target model; slice 98 adds `provider::resolve_route(Config,
> route_name)`, which resolves config `profiles` / `routes` into the existing
> `provider::Route` value, preserves fallback order, maps provider aliases and
> exact `ProtocolKind` spellings, and reports config errors for missing profile
> references or unknown provider spellings; slice 102 lets profiles carry an
> explicit `protocol` field that overrides provider-alias inference so a
> self-hosted or custom vendor label can still use a shipped wire format; slice
> 103 adds `provider::resolve_route_profiles(Config, route_name)` so the future
> adapter factory can consume the same resolved route plus profile endpoint
> metadata (`provider`, `base_url`, `api_key_env`) without reading secrets; slice
> 104 adds `provider::make_adapter_construction_plan(resolution)`, which keeps
> each resolved profile target beside its protocol adapter-family name, derives
> the existing loop-facing route, and preflights non-empty endpoint metadata plus
> `http://` / `https://` base URLs while still avoiding secrets, transports, and
> concrete adapters; slice 105 adds `provider::resolve_adapter_credentials(plan)`,
> the explicit environment-secret read that future factories will call to pair
> plan targets with in-memory API-key strings while keeping error context
> non-secret; slice 106 adds `provider::make_adapter_system(credentials,
> factories)`, which consumes those credentials plus caller-registered protocol
> factories and returns a profile-routed `provider::System` while leaving retry
> / fallback in `provider::execution::Runtime`; slice 107 adds
> `provider::make_protocol_request(request, target)`, an offline mapper for
> Anthropic Messages and OpenAI Responses request JSON that maps tool-result
> data into protocol-specific shapes; slice 108 adds
> `provider::decode_protocol_response(body_json, target)`, the paired offline
> response decoder for text/thinking/tool-use blocks, usage counters, model ids,
> and stop reasons; slice 109 adds
> `provider::ProtocolTransportAdapterFactory`, which composes those mappers over
> an injected `ProtocolTransport` for Anthropic/OpenAI systems; slices 121-124
> add HTTP SSE plus Anthropic/OpenAI streaming decoders, and bootstrap owns the
> concrete `http::Client`-backed construction seam),
> the slice-17/18/19/20/21 `oran-tool` foundation (`tool::Registry`
> with `add` / `remove` / `find` / `catalog` / `dispatch`;
> `Registry::add` rejects malformed tool declarations, including
> invalid `input_schema_json` (parseable JSON Schema object plus
> common keyword-type checks, with heavy JSON isolated in
> `src/oran-tool/schema_validation.cpp`); slice 59 adds the
> deterministic `tool::CatalogRenderer` with sorted active blocks,
> deferred-tool index rows, and a bounded 256-entry rendered-block
> cache keyed by stable rendered-block fields plus renderer version;
> slice 60 moves the result envelope into
> `<oran/tool/output.hpp>` as `tool::Output { text, data_json,
> attachments, usage, is_error }`, keeps the public header
> `nlohmann`-free by storing structured payload bytes as a string,
> and copies `Output::usage` into `hook::ToolAfterPayload::usage`
> on successful dispatch; slice 65 copies successful `Output::data_json`
> into `hook::ToolAfterPayload::data_json`, with `hook::Bus` redacting
> it for every non-`trusted_local` sink; slice 66 adds
> `OutputCapOptions` / `OutputCapReport` / `apply_output_caps`, applies
> `DispatchContext::output_caps` after successful handler returns and before
> `tool_after`, and parses the matching `runtime.tool_output` config
> defaults for the future scheduler/agent owner; slice 61 starts the built-in migration by
> filling usage counters for `FileWrite`, `FileEdit`, and
> `FileDelete` while leaving `data_json` empty; slice 62 migrates
> `FileRead` by keeping the text header/body fallback while adding
> `data_json` (`kind`, `path`, requested `text`, `fingerprint`,
> `start_line`, `end_line`, `returned_bytes`, `truncated`) and usage
> counters (`bytes_read`, `files_touched`, `truncated`); slice 63
> migrates `FileSearch` by keeping the existing
> `path:line:text` text rendering and trailing truncation summary,
> filling `data_json` with `kind`, `path`, `pattern`, `regex`,
> `matches[]` (`{path, line_number, text}` per match), `match_count`,
> `truncated`, `truncation_reason` (null / `matches` / `bytes`),
> `files_scanned`, and `bytes_read`, and filling usage counters
> `bytes_read` (cumulative scanned file bytes), `files_touched`
> (non-binary scanned files), `match_count` (post-truncation), and the
> `truncated` cap flag; slice 64 migrates `DirectoryList` by keeping
> the existing `<path>:<kind>:<size>` text rendering, filling `data_json`
> with `kind`, `path`, `include_hidden`, `recursive`, `max_entries`,
> `entry_count`, and an `entries[]` array of
> `{name, path, kind, size_bytes}` (with JSON null for non-regular
> `size_bytes`), and filling usage `files_touched=1` plus
> `match_count=entry_count`; slice 264 extends `DirectoryList` with
> `recursive=true`, preserving the entries shape while walking a whole
> project tree, skipping nested symlinks, and always skipping `.git`,
> `.xmake`, `.orangutan`, `build`, and `node_modules` directories;
> `entries_` uses
> transparent string hashing so lookup by `std::string_view` in
> `remove`, `find`, and `dispatch` does not allocate a temporary key;
> `tool::Workspace` (slice 37) canonicalises tool-layer paths without
> exposing `<filesystem>` in the public header, and
> `DispatchContext::workspace` is the interim non-owning seam file
> built-ins consume until `tool::Runtime::workspace()` lands; slice 38
> adds `FileWrite` / `FileEdit` / `FileDelete` adoption through that
> seam, slice 39 adds `FileSearch` through `resolve_list`, and slice 40
> adds `DirectoryList` through `resolve_list` so every filesystem
> built-in now resolves at the handler entry. Slice 41 promotes
> `tool::Workspace` ownership into `bootstrap::RuntimeAssembly` and routes
> `permissions.workspace.extra_{read,write}_roots` from `oran-config`
> into `tool::WorkspaceOptions` so overrides canonicalise once at boot.
> Slice 55 moves path resolution to the registry boundary for known
> filesystem built-ins: `Registry::dispatch` pre-resolves `path` through the
> intent-specific `Workspace` method before permission evaluation, stores the
> absolute path on `DispatchContext::resolved_path` for handlers, writes
> redacted path-resolution metadata under
> `permission::AuditEvent::metadata_json`, and returns resolver failures
> before handlers run or ask-approval replay is spent.
> Slice 79 adds `DispatchContext::parent_turn_id` so direct dispatch audit
> rows and their post-result metadata updates carry the future trace join key
> when an agent turn supplies one.
> Slice 42 starts spec 0011 with `io::FileFingerprint` (`size_bytes`,
> `mtime_ns`, reserved `optional<string> sha256`) plus a synchronous
> `io::compute_file_fingerprint(path)` helper — the lowest-cost identity
> primitive future range-read and `if_version` slices consume.
> The
> dispatch path runs `RuleSet::evaluate` against the call's
> `ToolDef::required_capabilities`, records one
> `permission::AuditEvent` per call with
> `input_hash = SHA-256(input_json)` onto the supplied `AuditSink`,
> then branches `allow` → handler, `deny` → `permission_denied`,
> `ask` → consults the optional `(ApprovalBroker*, ApprovalToken*)`
> pair carried on `DispatchContext` and remaps the audit outcome
> to `approved` (runs handler) or `rejected` (forwards the
> broker's `reason` context entry verbatim); when no broker or
> no token is supplied the short-circuit `permission_denied` /
> `reason=approval_required` path is preserved but the error
> now also carries `decision_reason` + `replay_max` +
> `approval_ttl_seconds` so the agent loop can hand them
> straight to `ApprovalBroker::approve`; built-ins `FileRead`
> (slice 17, `tool::register_file_read`; slice 37 resolves the input
> through `tool::Workspace::resolve_read` when `DispatchContext::workspace`
> is supplied; slice 45 extends the schema to the v2 shape `{path,
> start_line?, line_count?, offset_bytes?, length_bytes?, max_bytes?,
> if_version?}`, dispatches through `io::read_text_file_ranged`,
> wraps the body in a `<path>:<start>-<end> fingerprint=<token>
> bytes=<n>[ truncated]` header line, and short-circuits unchanged
> files to the new `Error::not_modified` kind when the opaque
> version token `v1:<sha256(canonical_path)>:<size>:<mtime_ns>`
> still matches the current fingerprint; slice 46 lifts the token
> helper into a private `src/oran-tool/version_token.hpp` shared
> with the mutation built-ins; slice 62 also returns the same payload in
> `Output::data_json` as a JSON object with the requested text and fills
> `Output::usage.bytes_read`, `files_touched`, and `truncated`),
> `FileWrite` (slice 18,
> `tool::register_file_write`, capability `write_file`, input
> `{path, content, mode?, create_parents?, max_bytes?, expected_version?}` with
> `mode ∈ {truncate (default), append, fail_if_exists}` and a
> 16 MiB default / hard ceiling on `max_bytes`; slice 38 resolves
> through `tool::Workspace::resolve_write` when supplied; slice 46
> guards mutations behind an optional `expected_version` token —
> a mismatch (or a vanished target) aborts the write before any
> bytes hit disk as `Error::conflict` with `reason=stale_fingerprint`
> and the current `fingerprint` in context; slice 61 fills
> `Output::usage.bytes_written` and `files_touched` on success),
> `FileEdit`
> (slice 19, `tool::register_file_edit`, capability `edit_file`,
> input `{path, old_string, new_string, replace_all?, max_bytes?, expected_version?}` —
> `conflict` if `old_string` is not unique unless `replace_all` is
> set, `not_found` if `old_string` is absent, and the read + final
> replacement output are capped by `max_bytes`; slice 38 resolves
> through `tool::Workspace::resolve_write` when supplied; slice 46
> guards the edit behind the same optional `expected_version`
> contract as `FileWrite`; slice 61 fills
> `Output::usage.bytes_read`, `bytes_written`, `files_touched`,
> and `match_count` on success), `FileSearch`
> (slice 20, `tool::register_file_search`, capability `read_file`,
> input `{path, pattern, max_matches?, include_hidden?, regex?,
> max_output_bytes?, respect_ignore?}` —
> literal substring by default, or re2 partial-match when
> `regex=true` (slice 24, via `permission::InputPattern`);
> single-file or recursive directory walk via
> `std::filesystem::recursive_directory_iterator`; binary
> heuristic skips NUL-bearing files during walks; dotfile-skip by
> default; slice 47 adds an optional `max_output_bytes` field
> (default 1 MiB) that caps the rendered `path:line:text` payload —
> when the byte cap fires first the trailing summary spells
> `(truncated; output capped at <N> bytes)`, otherwise the legacy
> `(truncated; matches capped at <N>)` message wins so the
> match-count cap always dominates a tie; slice 48 adds a built-in
> ignore predicate that skips `.git`, `.xmake`, `.orangutan`,
> `build`, and `node_modules` directories regardless of
> `include_hidden` (so an opt-in to scan hidden files still does
> not unleash a full descent through `.git/`), with an optional
> `respect_ignore=false` field for forensic searches; slice 49
> honours `.gitignore` / `.ignore` files from the search root
> downward for the common Git-style subset (`#` comments, blanks,
> escaped leading `#` / `!` literals, `!` negation, trailing `/`
> directory rules, slash-relative patterns, basename patterns, and
> fnmatch-style globs); slice 51 adds a bounded compiled-regex cache
> (`core::BoundedCache`, 64 entries / 64 KiB / 10-minute TTL) so
> repeated `regex=true` searches with the same pattern reuse the
> compiled `permission::InputPattern`;
> ripgrep-class
> optimisations deferred to follow-up
> slices tracked in `exec-plans/tech-debt-tracker.md`;
> slice 39 resolves the root path through `tool::Workspace::resolve_list`
> when `DispatchContext::workspace` is supplied),
> `DirectoryList` (slice 29, `tool::register_directory_list`,
> capability `list_directory` (new in `core::Capability`), input
> `{path, include_hidden?, recursive?, max_entries?}` — single-level
> enumeration through `oran-io::list_directory` by default, or a
> slice-264 whole-tree recursive listing when `recursive=true`;
> renders one
> `<path>:<kind>:<size_bytes or '-'>` line per entry sorted by
> path, the literal text `no entries` when the directory is empty
> after the hidden filter, and propagates the `io: directory
> entry limit exceeded` error verbatim when `max_entries` is
> exceeded — raise the cap and retry; slice 40 resolves through
> `tool::Workspace::resolve_list` when supplied; recursive listings
> skip nested symlinks and always skip `.git`, `.xmake`,
> `.orangutan`, `build`, and `node_modules` directories), and `FileDelete`
> (slice 30, `tool::register_file_delete`, capability
> `delete_path` (first built-in that exercises this slice-7
> capability), input `{path}` — refuses non-regular files
> (directories and symlinks reject as `invalid_argument` from
> `oran-io::delete_file`), `not_found` when the file does not
> exist, and returns the literal success message
> `deleted <path>`; slice 38 resolves through
> `tool::Workspace::resolve_delete` when supplied; slice 61 fills
> `Output::usage.bytes_written=0` and `files_touched=1` on
> success; future direction is a unified delete tool
> covering both files and folders, not per-kind splits)),
> and the slice-22 `oran-hook` foundation (`hook::Event` enum
> covering the 41 lifecycle events the design contemplates,
> `hook::Mode { advisory, blocking }` + `default_mode(Event)`
> annotation, abstract `hook::Sink` with `SinkKind::default_` /
> `SinkKind::trusted_local`, `hook::InProcessSink`
> `std::function`-backed implementation with optional sink kind, and
> `hook::Bus` with
> bind / unbind / `publish_advisory` — the bus starts subscribed
> sinks as sibling child coroutines, keeps returned outcome rows
> in subscription order, shares at most one raw payload snapshot and one
> default/redacted snapshot per publish, clears raw `ToolAfterPayload::data_json`
> for non-trusted-local sinks, and slice 152 substitutes optional redacted
> mutation `input_json` summaries for non-trusted sinks while preserving
> original input for trusted-local sinks;
> then captures
> per-sink `Result<void>` in
> `PublishOutcome`, and never aborts on a sink error; slice 90
> adds `Bus::publish_blocking<E>`, slice 91 makes
> `Registry::dispatch` consume blocking `tool_before` decisions, and
> slice 92 adds the per-sink blocking timeout policy.
> `Registry::dispatch` grew an optional `DispatchContext::bus`
> (`hook::Bus*`) that publishes blocking
> `hook::Event::tool_before` before workspace pre-resolution and
> permission evaluation, consuming veto / rewrite /
> require_approval decisions and serializing consulted sink
> decisions into audit metadata. It then publishes
> `hook::Event::tool_dispatched` between audit success and the
> handler co_await on the paths where the handler will actually
> run (slice 25 — `allow` or ask-approved, carrying the rule
> verdict wire spelling),
> `hook::Event::tool_error` on every error exit (slice 25 — handler
> failure, permission deny, broker rejection, audit error, ask
> short-circuit; failure-only narrow channel), and
> `hook::Event::tool_after` at every exit path — the later three
> remain advisory, while `tool_before` is now the blocking control
> point),
> and the slice-72/slice-80 first `oran-agent` surfaces (`agent::SessionState`
> owns `prompt::PromotionState`, observes successful `ToolSearch`
> structured output, promotes deferred matches into the next prompt
> snapshot, ignores failed/non-search outputs, rejects malformed
> successful structured data without partial mutation, and keeps
> `nlohmann_json` isolated in `src/oran-agent/session_state.cpp`;
> `agent::Loop` now builds the rendered prompt, maps cache hints and active
> tools into `provider::Request`, drives a supplied `provider::System`,
> and, when supplied a `tool::Registry` plus `tool::DispatchContext`,
> sequentially dispatches `tool_use` blocks, appends ordered tool results,
> re-enters the provider until a terminal turn or iteration cap, tags
> parent-cancelled provider awaits with `provider_initial`,
> `provider_stream`, or `provider_complete` and tool awaits with `tools`, and writes
> cancelled trace rows when trace is configured; slice 79
> threads `RunTurnInputs::turn_id` into direct dispatch audit rows when
> tracing is enabled, slice 85 generates that id when a trace writer is
> configured and callers leave it unset, clears/restores reusable context ids
> when tracing is explicitly disabled, writes one body-free `trace_turns` row
> before returning terminal-success responses, and writes provider/tool cancelled rows
> before returning parent-cancelled errors; slice 96 also refreshes
> `DispatchContext::now` around direct dispatch so broker-backed
> `permission_ask_rendered` prompts use a real per-call timestamp, and slice
> 155 exposes `DispatchContext::for_now(...)` so bootstrap and the scheduler
> share that current-clock context construction instead of copying field lists)
> are implemented.
> Rows whose purpose still says "planned" will land per `docs/exec-plans/` as
> future slices are scheduled. The build system, PCH, tests bucket, and bench
> bucket conventions are live; see the history entries under
> `docs/histories/2026-05/`.

| Library              | Purpose                                         | Depends on (allowed)                          |
| -------------------- | ----------------------------------------------- | --------------------------------------------- |
| `oran-core`          | `Result<T>`, `Error`, `Time` + ISO-8601 UTC helpers, `Role`, `StopReason`, `Content` variant, `Message`, `ToolDef` (with `required_capabilities`, `deferred`, and `category`), `core::str` UTF-8 helpers, `Capability` vocabulary (21 enumerators, slice 29 adds `list_directory`, slice 147 adds `deactivate_skill`), `core::TurnId` (slice 79 shared 16-byte trace/audit correlation id), and (slice 44) the generic `core::BoundedCache<Key, Value>` primitive (LRU on access, insert-based TTL, byte-budget eviction, explicit `erase_if` invalidation, customizable byte-size functor via template parameter, `Stats` accessor exposed for the future `oran-log`) | stdlib only |
| `oran-async`         | asio `Runtime` (blocking `run()` + non-blocking `start()` for embedding under a foreign UI loop, slice 251), `Awaitable<T>`, bounded `Channel<T>` with non-blocking `try_send` / `try_receive`, cancel-aware `sleep_for`; mailbox policy lands in orchestration | `oran-core`, asio |
| `oran-log`           | spdlog shim + secret redaction; thread-local context | `oran-core`, spdlog/fmt |
| `oran-io`            | file/directory IO MVP — `read_text_file`, `write_text_file`, `list_directory`, `delete_file` (slice 30, regular-file only), (slice 42) `io::FileFingerprint` + `io::compute_file_fingerprint`, (slice 43) the range-aware `io::read_text_file_ranged` returning `ReadTextResult { text, fingerprint, start_line, end_line, returned_bytes, truncated }` with `FileRange { LineSpan | ByteSpan }` input validation, mid-read fingerprint capture (size/mtime drift -> retry once for whole-file reads under 64 KiB, surface `Error::conflict` for larger or ranged reads), dual-end UTF-8 code-point boundary alignment for byte ranges, (slice 50) a bounded `core::BoundedCache`-backed line-offset index for line ranges in files larger than 256 KiB, (slice 52) a bounded file-view cache for successful `ReadTextResult` payloads keyed by canonical path + range + max-bytes + cheap fingerprint, (slice 54) public `ReadTextFileCacheStats` for those two caches, (slice 57) public `invalidate_read_text_file_ranged_cache(path)` for path-scoped invalidation, (slice 58) `watch_read_text_file_ranged_cache(executor, root, options)` for cancel-aware Linux/inotify external-edit cache invalidation with aggregate `ReadTextFileWatchStats`, (slice 53) a bounded 64-entry singleflight table for concurrent cold `read_text_file_ranged` calls with public `ReadTextSingleflightStats`, (slice 153) `WriteTextDurability { rename_only, fsync_file, fsync_file_and_parent }` plus PID+random exclusively-created temp leaves for atomic writes, and (slice 154) `io::run_blocking(executor, fn)` for short cancel-aware blocking callables returning `core::Result<T>`; both caches invalidate the affected canonical path after successful in-process writes/deletes / watcher events and file-view hits revalidate with `stat` before returning; planned glob, pipe, subprocess, signal, content hashing, and bootstrap/config startup wiring for long-lived watchers | `oran-core`, `oran-async` |
| `oran-http`          | libcurl-backed HTTP client: `<oran/http.hpp>` exports stdlib-shaped `Header`, `BodyRequest`, `BodyResponse`, `SseEvent`, and pimpl-backed `http::Client` with `send` (collect one body) and (slice 121) `send_streaming(BodyRequest, SseEventCallback)` for Server-Sent Events: an internal incremental `text/event-stream` parser (`src/oran-http/_impl/sse_parser.hpp`) runs inside the libcurl write callback on the blocking executor, and each decoded `SseEvent` is `asio::post`-ed through a strand on the caller's coroutine executor before the sink runs so stateful decoders observe one event at a time (the decoder/sink never run on the curl thread); a 2xx `text/event-stream` response resolves with status + headers and an empty body, any other response is collected into the body for the caller to decode, and the existing 50 ms curl poll surfaces mid-stream cancellation; callers provide the executor used for blocking curl work, curl handles remain private, and server/router surfaces remain planned. (Slice 231) `<oran/http/websocket.hpp>` adds `http::WebSocket` (`connect`/`receive`/`send_text`/`close`, with `WsConnectRequest`/`WsMessage`/`WsMessageKind`) over libcurl connect-only WebSocket mode: the handshake runs in non-blocking `curl_multi_perform` rounds and receive/send suspend on asio socket readiness (a `dup` of curl's active socket), so an idle connection holds no CPU-pool thread; `close()` completes the RFC 6455 closing handshake (drain to peer close/EOF, bounded). The shared libcurl RAII wrappers + error translation live in `src/oran-http/_impl/curl_common.hpp`, consumed by both `client.cpp` and `websocket.cpp` | `oran-core`, `oran-async`, libcurl |
| `oran-storage`       | SQLite expected-only connection/statement core with text/int/double/null plus BLOB bind/read support, optional per-open SQLite auto-extension registration for gated adapters, migration runner with SQL-file loading **and compile-time-embedded built-in migrations** (`built_in_audit_migrations()` / `built_in_session_migrations()` / `built_in_trace_migrations()` reach the SQL via C++26 `#embed`; trace is audit DB migration version 2, audit parent-turn correlation is version 3, audit event-kind discrimination is version 4, and audit tool-call rollups are version 5), async writer/reader `Pool` with per-slot `StatementCache` and the same optional auto-extension registration passed to every writer/reader connection, standalone per-connection `StatementCache`, `SessionRepository` (typed `core::Role` at the message API boundary plus slice-148 `session_skill_activations` upsert/load rows for latest per-session skill active/inactive state), `AuditRepository` (typed audit-event append/update-metadata/list/count over `audit_events`; slice 67 adds same-row metadata replacement for post-result usage enrichment, slice 79 adds nullable `parent_turn_id` BLOB matching so usage enrichment cannot cross same-tool traced turns, slice 88 adds the operator-level `list_events_for_turn(TurnId, limit)` read that joins by `parent_turn_id` ordered `id ASC` so the spec-0017 multi-tool dispatch order survives the trace/audit join, slice 93 adds `event_kind` on append/update/list records so permission-decision rows and `hook_publish` rows can share the same table without metadata-update clobbering, and slice 243 adds `audit_tool_call_rollups` plus `list_tool_call_rollups(...)` for per-turn/per-tool decision/hook counts and optional usage latency samples), and `TraceRepository` (typed spec-0018 `trace_turns` append/get/list/count plus provider usage rollups and slice-150 explicit-cutoff purge over `core::TurnId` / BLOB turn/session ids, prompt/cache hashes, usage/cost fields, optional cancellation phase, and redacted context bytes; slice 80 adds the first `oran-agent` terminal-success writer, slice 82 makes the loop skip that writer when tracing is explicitly disabled, slice 83 adds provider/tool cancellation-row writes, slice 84 adds provider/loop-boundary error-row writes, slice 85 lets the loop generate missing trace turn ids, slice 127 adds UTC-day/agent/profile/model usage aggregation over recorded trace rows, and slice 150 deletes only `trace_turns` older than a caller-provided `started_at_ns` cutoff); memory-tier schemas now live above the pool in `oran-memory`; slice 189 adds the automation retention job/run repository above the same generic pool in `oran-automation`, slice 190 adds the caller-driven service tick above that repository, slice 191 adds optional advisory hook publishing above the same automation service, slice 192 adds the caller-owned `AutomationRuntime` open/migrate handle above storage, slice 193 adds the caller-started retention loop step above that runtime/service boundary, slice 194 adds job lifecycle hook publishing above the same automation service/runtime state, slice 195 adds automation-owned retention lease rows above the same generic pool, slice 196 adds finite caller-owned loop policy above the same automation-owned repository/service stack, slice 198 adds automation-owned cron job rows (with required prompt input as of slice 219), slice 206 adds automation-owned cron run rows, slice 209 adds automation-owned cron execution lease rows, slice 210 adds automation-owned cron agent lease rows, slice 211 adds automation-owned triggered job descriptor rows (with required prompt input as of slice 219), slice 212 adds automation-owned triggered run rows, and slice 214 adds automation-owned triggered agent lease rows above the same generic pool | `oran-core`, `oran-async`, sqlite3 |
| `oran-config`        | JSON config loader with typed runtime/profile/route/session/desktop/trace/hooks/memory/automation fields, including `runtime.tool_output.max_text_bytes` / `max_data_bytes` defaults for structured tool-output caps, `runtime.prompt.active_tools` as either `"defaults"` or an explicit active-tool allowlist consumed by `oran-prompt`, slice-102 optional `profiles.<name>.protocol` strings for provider route resolution, slice-129 optional `profiles.<name>.pricing` USD-per-million-token fields consumed by provider route resolution and the agent loop's cost estimate, slice-81 `trace.enabled` / `trace.store_raw_bodies` / `trace.retention_days` defaults for spec-0018 trace policy (with slice-150 bootstrap consumption of `retention_days` for trace-row purge), slice-92 `hooks.timeout_ms` for the blocking hook deadline, slice-165 `memory.longterm.recall.enabled` / `limit`, slice-166 optional `memory.longterm.recall.kinds`, slice-167 `memory.longterm.recall.query_strategy` for ordinary configured-route prompt-boundary recall policy, slice-174 `memory.longterm.hybrid_search` parser policy (`enabled`, per-backend/result limits, lexical/vector weights) and slice-178 configured-route consumption under `--vector_memory=y` (default builds reject `enabled=true` with `reason=build_option_disabled`, `option=vector_memory`), slice-183 `memory.longterm.retention` parser policy (`forget_after_unused_days`, `importance_floor`, `max_records_per_scope`, `decay_check_interval_hours`) and slice-184 configured-route startup consumption for one bounded long-term decay pass, plus slice-203 `automation.cron.jobs[]` schedule seed parsing (`job_key`, optional `agent_key` defaulting to `automation`, required `agent_prompt`, POSIX cron `expression`, UTC `first_fire_at`, optional UTC `last_fired_at`) and slice-257 `automation.triggered.jobs[]` descriptor seed parsing (`job_key`, `trigger_key`, optional `agent_key` defaulting to `automation`, required `agent_prompt`) for bootstrap mapping into automation repository seed descriptors, slice-228 typed `channels[]` parsing (unique non-empty `id`, non-empty `kind`, optional non-empty `agent_key` defaulting to `default`, optional positive `inbound_capacity` defaulting to 64) plus slice-235 QQ channel metadata (`qq_app_id_env`, `qq_client_secret_env`, optional `qq_token_url` / `qq_api_base_url`, required `qq_gateway_url` when `kind == "qq"`) for bootstrap channel registration/routing, slice-249 typed `desktop` UI-preferences (`enabled`, `theme` ∈ {`system`, `light`, `dark`}, `reduce_motion`) replacing the legacy networked `web` block, env substitution, the typed `permissions` + `agents.<name>.permissions` overlay surface (layer-2/3 data of the three-layer rule merge), slice-139 optional `agents.<name>.skills_enabled` allowlists plus slice-146 optional `agents.<name>.skills_deactivated` names and `agents.<name>.skills_expirations` `{name, expires_at}` rows (the per-agent skill activation-policy inputs the bootstrap prompt runner maps into `skill::ActivationPolicy`) consumed by bootstrap prompt runners, and (slice 41) `permissions.workspace.extra_{read,write}_roots` parsed onto `WorkspacePermissionsConfig` for the bootstrap-owned `tool::Workspace`; planned schema + secret-protected fields plus typed hook sink/binding models | `oran-core`, `oran-storage` |
| `oran-permission`    | foundation rule evaluator: `Verdict`, `Mode`, `Rule`, `RuleSet`, `Decision`, `*`-glob tool matching, capability-aware gating (`Rule::capability` of `core::Capability`), the `Defaults::for_mode` baseline factory, the three-layer `materialize(Mode, global, per_agent)` merge that concatenates defaults + global config + per-agent overlay, the `ApprovalSecret` / `ApprovalAuthority` / `ApprovalToken` / `ApprovalBroker` ask-flow surface (including the slice-56 64-live-grants-per-identity cap), and the `AuditEvent` / `AuditMetadataUpdate` / `AuditSink` / `StorageAuditSink` audit pipeline (`record` plus slice-67 same-row metadata update; slice 79 carries optional `parent_turn_id` through record/update paths; slice 91 adds `AuditOutcome::blocked_by_hook` and `AuditOutcome::rewritten` for blocking hook decisions; slice 93 carries `event_kind` through record/update paths so `hook_publish` rows cannot be mistaken for ordinary permission decisions); planned re2 input regex extensions and bootstrap wiring | `oran-core`, `oran-config`, `oran-storage`, `oran-async` |
| `oran-skill`         | skill catalog renderer + section-4 owner plus markdown skill snapshot/runtime refresh support: `SkillDocument`, `SkillMetadata`, `Loader`, `load_directory`, and `load_catalog` snapshot `<workspace>/.orangutan/skills/*.md`, while slice 138 adds `WorkspaceSkillSnapshot` as a prompt-boundary owner that reloads the rendered catalog plus invocation document vector together, uses Linux inotify when available, falls back to a bounded content-aware directory signature before each prompt, and invalidates `oran-io` file-view cache entries before changed skill markdown is re-read; slice 142 adds `ActiveSkill` plus versioned `SkillInvoke` activation metadata helpers, slice 143 adds `ActivationPolicy` / `resolve_active_skills(...)` so the transcript-derived next-prompt marker policy is owned by `oran-skill`, the follow-up doc slice records the section-4 cache semantics future expiration/deactivation rules must preserve, slice 144 adds explicit `ActivationPolicy::deactivated_skill_names` as a deterministic prompt-boundary subtraction from active markers, slice 145 adds explicit `SkillExpiration` rows plus caller-supplied `ActivationPolicy::evaluation_time` for deterministic prompt-boundary expiry, slice 147 adds `render_deactivation_data_json` / `deactivated_skill_from_data_json` plus an order-aware `active_skills_from_transcript` that nets transcript `SkillDeactivate` deactivations against `SkillInvoke` activations (most recent event wins), slice 148 adds `SessionSkillActivation` rows on `ActivationPolicy` as a durable overlay that can add/remove markers after transcript scanning and before config deactivation/expiration subtraction, and slice 149 adds `SkillActivationEvent` / `skill_activation_events_from_transcript(...)` so runtime owners can persist semantic activation/deactivation events without duplicating transcript scans; skill invocation itself remains owned by `oran-tool` plus bootstrap's callback boundary | `oran-core`, `oran-async`, `oran-io` |
| `oran-tool`          | tool registry (`Registry::add` validates declarations and `input_schema_json` before insertion; `Registry::entries_` uses transparent string lookup for `std::string_view` names), `tool::CatalogRenderer` (deterministic prompt-facing catalog renderer: sorted full-schema active blocks, deferred name/description rows, and aggregate stats for a bounded 256-entry rendered-block cache keyed by rendered-block fields; consumed by `oran-prompt` for section 2/3 bytes), `tool::Output` / `ToolUsage` / `Attachment` (required text fallback, optional serialized `data_json`, metadata-only attachments, usage counters and cap flags; `Output::text_only` preserves the v1 text path) plus `OutputCapOptions` / `OutputCapReport` / `apply_output_caps` for spec-0014 text/data byte caps, `tool::Workspace` path resolver (`resolve_read` / `resolve_write` / `resolve_delete` / `resolve_list`, with canonical roots, traversal rejection, symlink policy, and extra read/write roots), registry-boundary workspace pre-resolution for known filesystem built-ins (`DispatchContext::resolved_path`, redacted `metadata_json.path_resolution`, resolver failures audited before handler execution / ask replay), transient dispatch registry context (`DispatchContext::registry` is set to the currently dispatching registry and restored on exit so metadata tools can inspect the live catalog without self-capturing a movable registry), slice-155 `DispatchContext::for_now(...)` factories for fresh current-clock context construction and prototype clone/refresh before concurrent scheduler calls, dispatch through `permission::RuleSet` + `permission::AuditSink` with the slice-21 `(ApprovalBroker*, ApprovalToken*)` mediation of `Verdict::ask` (audit outcome promoted to `approved` on broker.check OK, `rejected` on broker rejection — `expired` / `tool_mismatch` / `identity_mismatch` / `input_mismatch` / `mac_mismatch` / `no_grant` / `replay_exhausted` reason forwarded; the short-circuit `approval_required` path carries `decision_reason` / `replay_max` / `approval_ttl_seconds` in the error context) and the slice-22+25+60+65+66+67+79+91+152 `hook::Bus*` / output-boundary / audit-metadata tap (publishes blocking `tool_before` before workspace resolution and permission evaluation, consuming veto / rewrite / require_approval decisions and writing `metadata_json.hook_decisions`; publishes advisory `tool_dispatched` on `allow` / ask-approved paths, `tool_error` on error exits, and `tool_after` at every exit; successful dispatch first applies `DispatchContext::output_caps`, then best-effort enriches the same audit row's `metadata_json.usage`, with optional `DispatchContext::parent_turn_id` carried into both the decision row and metadata update for trace correlation, `tool_after` receives capped `Output::text`, `Output::usage` metrics, and raw `Output::data_json` when retained, with the bus redacting `data_json` for non-trusted-local sinks, and `FileWrite` / `FileEdit` payloads carry hash-and-byte-count `redacted_input_json` summaries for non-trusted sinks), built-ins `FileRead` (range-aware schema `{path, start_line?, line_count?, offset_bytes?, length_bytes?, max_bytes?, if_version?}` through `io::read_text_file_ranged`; `if_version` can short-circuit to `Error::not_modified`; success keeps the text header/body fallback, fills `data_json` with requested text plus range/fingerprint metadata, and fills usage bytes-read + file-count + truncation flag), `FileWrite` (`{path, content, mode?, create_parents?, max_bytes?, expected_version?}`; 16 MiB cap; stale `expected_version` fails with `reason=stale_fingerprint`; success fills usage bytes-written + file-count), `FileEdit` (`{path, old_string, new_string, replace_all?, max_bytes?, expected_version?}`; stale token guard; success fills usage bytes-read/written + file-count + match-count), `FileSearch` (`{path, pattern, max_matches?, include_hidden?, regex?, max_output_bytes?, respect_ignore?}`; regex compile cache, output cap, built-in skips, `.gitignore` / `.ignore` subset; success keeps the `path:line:text` text rendering and fills `data_json` with kind, path, pattern, regex, matches[], match_count, truncated, truncation_reason, files_scanned, bytes_read plus usage bytes-read + file-count + match-count + truncated flag), `DirectoryList` (`{path, include_hidden?, recursive?, max_entries?}`; success keeps the `<path>:<kind>:<size>` text rendering and fills `data_json` with kind, path, include_hidden, recursive, max_entries, entry_count, and an `entries[]` array of `{name, path, kind, size_bytes}` (size_bytes null for non-regular); `recursive=true` walks the whole tree without following nested symlinks and skips `.git`, `.xmake`, `.orangutan`, `build`, and `node_modules`; usage reports `files_touched=1` for single-level listings or root-plus-entry count for recursive listings and `match_count=entry_count`), `FileDelete` (`{path}` regular-file-only; success fills usage bytes-written zero + file-count), and `ToolSearch` (`{name?, category?, capability?}` metadata lookup; no required capability; success returns text plus `data_json` with query, match_count, and matched tool definitions including nested `input_schema`, required capabilities, deferred flag, and nullable category, with usage `match_count`; `agent::SessionState` consumes successful payloads for deferred-tool prompt promotion); slice 107 provider request serialization consumes `data_json` for OpenAI Responses function-call output JSON strings, while Anthropic Messages now sends the text fallback (or serialized data bytes as plain text when no fallback exists) for tool-result content; the permissioned skill built-ins `SkillInvoke` (`invoke_skill`) and slice-147 `SkillDeactivate` (`deactivate_skill`) delegate the loaded-skill lookup through `DispatchContext::skill_invoke` / `skill_deactivate` callbacks so `oran-tool` stays independent of `oran-skill`; slice 168 adds the deferred `MemoryRecall` built-in (`read_memory`) that parses `{query, limit?, kinds?}` and delegates through `DispatchContext::memory_recall` so `oran-tool` stays independent of `oran-memory`, returning deterministic recall text, `data_json.kind=memory_recall`, record metadata, and `usage.match_count`; slice 169 adds the deferred `MemoryRemember` built-in (`write_memory`) that parses `{id, kind, title, body, importance?, tags?, linked_record_ids?, shadow?}` and delegates through `DispatchContext::memory_remember`, returning confirmation text, `data_json.kind=memory_remember`, saved record metadata, and `usage.bytes_written`; slice 170 adds the deferred `MemoryForget` built-in (`write_memory`) that parses `{id}` and delegates through `DispatchContext::memory_forget`, returning confirmation text, `data_json.kind=memory_forget`, scoped removed-key metadata, and `usage.bytes_written=0`; `tool::Runtime` remains planned | `oran-core`, `oran-async`, `oran-permission`, `oran-hook`, `oran-io` |
| `oran-hook`          | hook bus + sink kinds: `Event` enum (41 lifecycle events), `Payload` variant plus shared immutable `PayloadPtr`, `Sink` abstract interface with `SinkKind::default_` / `SinkKind::trusted_local` plus a virtual `handle_blocking(Event, PayloadPtr) -> Awaitable<Result<HookDecision>>` defaulting to `proceed` (body in `src/oran-hook/sink.cpp`), `BusOptions { blocking_timeout }`, `Bus` with `bind` / `unbind` / `publish_advisory` (advisory contract — sinks observe, never veto; subscribed sinks are started as sibling child coroutines and returned `PublishOutcome` rows stay in subscription order; the bus shares at most one raw payload snapshot and one default/redacted snapshot per publish, clearing `ToolAfterPayload::data_json` and substituting optional `redacted_input_json` for non-trusted-local sinks; sink exceptions are caught and surfaced as `Error::internal` in `PublishOutcome`) plus `publish_blocking<E>` (constrained by `requires HasBlockingDecision<E>`; walks subscribed sinks in subscription order, records `HookDecision::trace` for every consulted sink, short-circuits at the first non-`proceed` decision, applies the same shared redaction snapshots the advisory path uses, converts sink `core::Result` errors / thrown exceptions into a veto with `reason="hook_error: <message> [sink=<id>]"`, and races each sink against `blocking_timeout`, synthesizing `reason=hook_timeout` with `HookDecisionTrace::elapsed` on expiry; with no sinks subscribed or every sink returning `proceed`, yields a proceed decision), `HookDecision { kind, reason, optional<string> rewritten_input_json, optional<core::Time> approval_expires_at, vector<HookDecisionTrace> trace }` and `HookDecisionKind { proceed, veto, rewrite, require_approval }` in `<oran/hook/decision.hpp>`, `EventTraits<E>` (empty primary template; explicit specialisations setting `Decision = HookDecision` for the spec-0015 v1 whitelist `tool_before` / `permission_ask_rendered` / `memory_write_before`) plus the `HasBlockingDecision<E>` concept in `<oran/hook/event_traits.hpp>`, `InProcessSink` (std::function-backed `PayloadPtr` `Callback` + optional `BlockingCallback` via `set_blocking_handler`), and typed payloads for tool lifecycle, direct permission asks, slice-126 provider lifecycle metadata, slice-179 memory write/delete payloads (`MemoryWritePayload` / `MemoryForgetPayload`), slice-180 memory read payloads (`MemoryReadPayload` / `MemoryReadHitPayload`) with default-sink redaction of memory record content, recall queries, and recalled hit content, slice-186/slice-191 memory decay metadata (`MemoryDecayPayload`) for startup and caller-driven periodic retention passes with no record content, and slice-194 automation job lifecycle metadata (`JobLifecyclePayload`) for start/outcome timing with no job record contents, reused by retention, cron, and triggered producers; planned `ShellSink` / `WebhookSink` / `LuaSink` (`sol2` feature-gated) | `oran-core`, `oran-async` |
| `oran-memory`        | typed memory-layer owners: slice 130 ships `<oran/memory.hpp>` plus `memory::session::Store`, a wrapper over `storage::SessionRepository` that keeps storage JSON-opaque while privately serializing/deserializing `core::Message` blocks (`TextContent`, `ThinkingContent`, `ToolUseContent`, `ToolResultContent`) into versioned session-message JSON; it exposes typed `SessionId`, `AgentKey`, `ListSessionsOptions`, and `SessionSummary`, validates required ids, returns parsing errors for malformed stored rows, and has `test-memory` / `bench-memory` parity. Slice 148 adds typed `SkillActivationUpdate` / `SkillActivationRecord` wrappers over storage's `session_skill_activations` rows so bootstrap can persist latest active/inactive skill state per session. Slice 131 wires `RuntimeAssembly` to own the separate sessions DB pool/repository/store for configured-route startup, slice 132 wires `AgentPromptRunner` to load and append through that store, and slice 133 adds `memory::Framing` / `memory::FramingOwner` as the once-per-turn section-5 prompt memory owner that the runner renders before `agent::Loop`. Slice 160 adds the public long-term memory contract: `memory::longterm::RecordKind`, `RecordKey`, `Record`, `Query`, `SearchHit`, `WriteRequest`, `TouchRequest`, `Backend`, `VectorBackend`, vector request/result shapes, and validation helpers for record/search/touch/vector inputs; slice 182 extends that contract with `DecayRequest`, `DecayResult`, `Backend::decay(...)`, and decay request validation. Slice 161 adds `memory::longterm::Fts5Backend`, the default SQLite FTS5 lexical backend over `storage::Pool`, with embedded long-term migrations, scoped CRUD/search, kind/shadow filters, lexical scores, `not_found` get misses, and idempotent deletes. Slice 162 adds `memory::longterm::Runtime`, `RecallRequest`, `RecallResult`, and `render_recall_framing(...)` so a prompt-boundary owner can validate backend search, return hits, and render deterministic section-5 long-term-memory bytes before `agent::Loop`. Slice 163 wires `RuntimeAssembly` to own the configured-route `<workspace>/.orangutan/memory.db` pool, default `Fts5Backend`, and `longterm::Runtime`, while built-in no-provider startup leaves long-term memory disabled. Slice 164 adds opt-in prompt-boundary recall through `AgentPromptRunnerOptions::longterm_recall`, using the assembly-owned runtime to fill section 5 once from record-only recall framing before `agent::Loop`; slice 165 lets ordinary bootstrap config map `memory.longterm.recall.enabled` / `limit` into that option, slice 166 adds optional kind filters over `RecordKind` spellings, and slice 167 adds a query-strategy selector for current-prompt versus last-user-message search text; slice 168 adds `render_recall_data_json(...)` plus the read-only `MemoryRecall` tool binding through `DispatchContext::memory_recall`; slice 169 adds `render_remember_data_json(...)` for saved-record metadata consumed by the write-side `MemoryRemember` tool binding; slice 170 adds `render_forget_data_json(...)` for scoped removed-key metadata consumed by the delete-side `MemoryForget` tool binding; slice 171 adds the 10k-record FTS5 search bench baseline; slice 172 adds `memory::longterm::HybridSearchRequest` / `HybridRuntime`, which composes a lexical `Backend` with any `VectorBackend`, hydrates vector-only keys through `Backend::get`, ignores stale vector rows whose records are missing, and returns weighted deterministic `SearchHit` rows plus recall framing; slice 174 adds the parser-level `memory.longterm.hybrid_search` config policy that validates positive lexical/vector/result limits plus non-negative weights; slice 175 first added the configured-route hybrid-search guard; slice 176 adds `memory::longterm::SqliteVecBackend`, an optional `--vector_memory=y` sqlite-vec `VectorBackend` that registers the sqlite-vec auto-extension through `storage::Pool::open`, migrates a scoped cosine `vec0` table, validates dimensions, and keeps default builds dependency-free with config errors from vector operations; slice 177 extends the 10k-record `bench-memory` comparison with gated sqlite-vec vector-only (~3.03 ms / batch) and FTS5+sqlite-vec hybrid (~18.96 ms / batch) rows; and slice 178 adds deterministic local text/record embedding helpers plus gated configured-route hybrid recall wiring: `RuntimeAssembly` owns `.orangutan/memory-vectors.db` / `SqliteVecBackend` / `HybridRuntime` under `--vector_memory=y`, `AgentPromptRunner` routes enabled prompt-boundary recall and `MemoryRecall` through hybrid search, and `MemoryRemember` / `MemoryForget` mirror vector writes/deletes. Default builds reject enabled hybrid search with `reason=build_option_disabled`, `option=vector_memory`. Slice 179 wires bootstrap-owned write/forget lifecycle hooks for `MemoryRemember` / `MemoryForget`: blocking `memory_write_before` can veto a write before lexical/vector mutation, and advisory `memory_write_after` / `memory_forget` observe successful mutations. Slice 180 wires advisory `memory_read_after` observability for successful prompt-boundary long-term recall and `MemoryRecall` tool reads, including default-sink redaction of raw recall queries and recalled hit content while trusted-local sinks receive raw query/record data. Slice 181 adds `TouchRequest` / `Backend::touch` and wires ordinary and hybrid recall to advance returned records' `last_read_at` before framing; plain search remains read-only. Slice 182 adds the library-level decay shadow boundary: `Fts5Backend::decay(...)` marks scoped, visible records with `last_read_at < unused_before` and `importance <= importance_floor` as shadow in bounded batches, advances `updated_at` monotonically, syncs FTS shadow metadata, and returns the changed records. Slice 183 adds the parsed `memory.longterm.retention` policy contract in `oran-config`; slice 184 consumes it from configured-route bootstrap by applying one bounded startup decay pass before long-term prompt/tool reads, slice 185 exposes that startup pass shadow count through `RuntimeAssembly` diagnostics plus the startup banner, slice 186 adds startup `memory_decay` hook metadata for configured-route startup passes, slice 187 adds `oran-automation` planning that can turn the retention interval into a due-only `DecayRequest` without making memory depend upward on automation, and slice 188 has bootstrap map configured retention into a stored automation-owned job descriptor, slice 189 adds automation-owned retention job/run/last-fired persistence without moving periodic ownership into memory, slice 190 adds the caller-driven `MemoryRetentionService::tick(...)` boundary that consumes the stored job and invokes a supplied backend when due, slice 191 lets that explicit tick owner publish advisory periodic `memory_decay` metadata through a caller-supplied hook bus, and slice 192 adds the caller-owned `AutomationRuntime` state handle for explicit automation DB open/migrate ownership, slice 193 adds the caller-started retention loop step, slice 194 adds advisory job lifecycle metadata from due retention ticks, and slice 195 adds retention job leases plus loop-side due-run ownership, and slice 196 adds finite caller-owned loop policy without moving periodic execution into memory. Process service/timer ownership remains outside `oran-memory`. Shared/team memory storage, external semantic embeddings, blocking `memory_read_before`, automation service-loop ownership, and memory-write rewrite/annotation remain planned. | `oran-core`, `oran-async`, `oran-storage`, nlohmann_json; optional sqlite-vec under `--vector_memory=y` |
| `oran-provider`      | provider-domain + fake-provider + route-resolver + execution-runtime slice: `<oran/provider.hpp>` exports the slice-73 cache-mapping shapes (`Request`, `Response`, `Usage`, `RetryPolicy`, `PromptCacheHints`, `PromptCacheOptions`, `make_prompt_cache_hints(RenderedPrompt, options)`) plus the slice-74 system surface (`ProtocolKind`, `ModelTarget`, `ProviderPricing`, `Route`, abstract `provider::EventSink` with default no-op text/thinking/tool delta callbacks plus terminal `on_done`, abstract `provider::System::send(Request, Route, EventSink*) const`, and `provider::FakeProvider` — the first concrete `System`) plus slice-97 `provider::execution::Runtime`, route/profile resolution, adapter planning/credential resolution, protocol request/response mapping, and the injected `ProtocolTransport` seam; route resolution now carries optional per-profile pricing through `ModelTarget::pricing` for agent-loop cost estimation; `provider::ProtocolTransportAdapterFactory` builds Anthropic/OpenAI Responses `System` backends over an injected transport, injects provider API-key headers, maps HTTP status classes into provider error categories, streams either shipped protocol when `request.stream` is true and the transport supports streaming, and rejects selected route profile/model/protocol mismatches before sending; slice 111's `bootstrap::HttpProviderBackend` supplies the first `http::Client`-backed `ProtocolTransport` adapter outside `oran-provider`, slice 112 consumes it from configured-route `bootstrap::run`, and slices 122/124 add the `AnthropicSseDecoder` / `OpenAiResponsesSseDecoder` streaming assembly paths that fan ordered text/thinking/tool deltas through `provider::EventSink` while returning the same terminal `Response` shape; provider lifecycle hooks and profile-priced cost calculation are emitted/consumed by `oran-agent` so this target stays hook-free, and trace-derived provider usage rollups live in `oran-storage`; real vendor cache-control mapping remains planned | currently `oran-core`, `oran-async`, `oran-config`, `oran-prompt`; HTTP adapter wiring, hook publication, and storage rollups stay outside `oran-provider` |
| `oran-prompt`        | prompt builder + promotion state: `BuilderInputs`, `BuilderOptions`, `CacheSection`, `RenderedPrompt`, and `PromotionState`; assembles the seven prompt-design sections, consumes `config::PromptActiveToolsConfig`, delegates section 2/3 catalog bytes to `tool::CatalogRenderer`, consumes sorted promoted-tool snapshots to move deferred tools into the next active catalog, computes stable section hashes, a cache-versioned prefix hash over sections 1-6, prefix byte count, and one breakpoint before the conversation tail; `PromotionState` is explicit-time, session-owned, capped at 16 names with a 24-hour TTL, and exposes aggregate stats without prompt byte drift; real section owners now start outside `oran-prompt` with slice-133 memory framing, slice-134 system preamble, and slice-135-149 skill catalog rendering/loading/hot-reload/filtering/activation-deactivation-expiration plus durable session activation policy and shared activation-event extraction, with section-4 active-marker changes invalidating through content hash unless rendering/interpretation rules require a `skills_catalog` cache-version bump | `oran-core`, `oran-async`, `oran-config`, `oran-tool` |
| `oran-agent`         | first loop surfaces: `agent::SessionState` owns `prompt::PromotionState`, observes `ToolSearch` `Output::data_json`, promotes only deferred matches into the next prompt snapshot, ignores non-search / failed-search outputs, rejects malformed successful structured data without mutating state, and exposes promotion snapshots/stats/options; slice 134 adds `agent::SystemPreamble`, `agent::SystemPreambleOwner`, and `agent::default_system_preamble()` for stable section-1 bytes; `agent::Loop` owns the fake-provider-backed turn driver (`LoopOptions`, `RunTurnInputs`, `RunTurnResult`) that builds a `prompt::RenderedPrompt` from already-rendered stable section inputs (using the owned default preamble when `RunTurnInputs::system_preamble` is empty), maps prompt-cache hints, mirrors active/promoted tools into name-sorted `provider::Request::tools`, sends requests through `provider::System`, dispatches tool-use batches through `agent::ToolScheduler`, appends ordered tool results, re-enters the provider until a terminal text-style response or iteration cap, aggregates provider usage, computes missing provider cost estimates from selected `ModelTarget::pricing`, returns the complete transcript tail, forwards ordinary provider errors unchanged, converts model-repairable tool errors into tool-result error blocks, annotates parent-cancelled provider/tool awaits, writes trace rows when configured, and publishes slice-126 advisory provider lifecycle hooks through an optional `hook::Bus`; slices 116-120 complete the tool-scheduler v1 arc with bounded parallelism, per-canonical-path read/write locks, per-call timeouts, parent-cancellation fanout, `cancellation_lag` audit rows, benchmark coverage, and `AgentPromptRunner` ownership of scheduler config; slices 132-134 make the bootstrap runner load persisted conversation history through `oran-memory`, append successful transcript suffixes, render memory framing once before loop entry, and render system preamble once before loop entry. Slices 142-143 keep skill active-marker extraction and activation policy outside `oran-agent`; the loop only preserves successful tool `data_json` in transcript results. | `oran-core`, `oran-async`, `oran-storage`, `oran-prompt`, `oran-tool`, `oran-provider`, `oran-hook`, `oran-memory` |
| `oran-orchestration` | team + mailbox + coordination strategies | `oran-agent`, `oran-async` |
| `oran-automation`    | Automation runtime library: deterministic periodic/cron planning, retention planning/execution, automation DB repository/runtime ownership, durable cron and triggered descriptors/runs, cron/triggered/retention leases, caller-owned cron and triggered services/queues, prompt-runner adapters, notifier callbacks, explicit cron and triggered seed application, and the composed `AutomationService` finite loop. As of slice 257 the library exports `AutomationRuntime::apply_cron_job_seeds(...)` and `AutomationRuntime::apply_triggered_job_seeds(...)`; bootstrap/`--serve` may call those helpers, but the library still does not read config, own provider/agent construction, or start hidden background work. Slice 258 adds a bootstrap-owned channel producer over `AutomationService::enqueue_triggered(...)`; webhook producers and concrete cli/channel/desktop notifier routing remain downstream. | `oran-core`, `oran-async`, `oran-storage`, `oran-memory`, `oran-hook`; planned `oran-agent` |
| `oran-channel`       | channel adapter foundation: `<oran/channel.hpp>` exports `Capabilities`, inbound/outbound envelopes, the abstract `Channel` trait, and caller-owned `ChannelManager` registration/lifecycle/`receive_one`/send/capability APIs over bounded `async::Channel` fan-in; slice 227 adds the concrete in-process `MockChannel` ingress adapter (`push_inbound` external entry over a bounded queue, awaiting `next_message()`, lifecycle gating, recorded sends with deterministic receipts) plus the channel→agent dispatch seam (`ChannelPromptRunRequest` / `ChannelPromptRunResult`, the `ChannelPromptRunner` function contract, `make_prompt_run_request(...)`, `make_reply_message(...)`, and caller-owned `dispatch_one(manager, runner)`); slice 256 tightens `MockChannel::stop()` so pending `next_message()` waits unblock as cancelled and `start()` reopens a fresh inbound queue, matching the lifecycle behavior `--serve` needs for adapter shutdown; slices 259-263 keep per-conversation scheduling policy, ordering, idle worker eviction, worker metrics observation/logging, and per-message deadline policy in the bootstrap service owner rather than moving them into this low-level manager; platform adapters beyond QQ/webhook remain downstream | `oran-core`, `oran-async` |
| `oran-channel-qq`    | QQ adapter (optional, gated behind `xmake f --channel_qq=y`, default off until the port's round-trip acceptance passes): slice 229 ships `<oran/channel-qq.hpp>` with `qq::TokenStore` (app-access-token fetch over the QQ token endpoint, refresh-ahead expiry caching with `expires_in` int-or-string coercion and a 60 s floor, single-flight refresh sharing one in-flight request's outcome across concurrent callers, `invalidate()` for the 401 path) and `qq::ApiClient` (authenticated `get`/`post`/`put`/`del` with `Authorization: QQBot <token>`, base-URL joining, the platform retry ladder — one token refresh on 401, bounded `retry-after`-honoring 429 retries, bounded exponential gateway backoff — and the pure `normalize_api_response` seam capturing `x-tps-trace-id` / `retry-after` headers and the `{"code","message"}` business envelope); JSON bodies cross the public API as serialized strings (C6). Slice 230 adds `qq::GatewaySession` (milestone 2a): the pure, offline-testable gateway protocol/session state machine — `consume(frame_json)` decodes one gateway text frame and returns a `GatewayReaction` (arm heartbeat timer, send Identify-vs-Resume, surface a non-lifecycle dispatch, reconnect resume/fresh, session-ready), caching the `s` seq cursor and READY `session_id`; `build_identify`/`build_resume`/`build_heartbeat` emit outbound payloads with the auth token injected per-call (session never stores the secret, C5); `classify_close_code` maps close codes to a recovery action with the SDK-grounded corrections (4009 = the only resume-able close; 4013/4014 = intents; 4914/4915 = fatal). Frames/payloads also cross as JSON strings (C6). Slice 232 adds `qq::GatewayTransport`, the caller-owned `http::WebSocket` driver: it sends Identify/Resume/heartbeat payloads with `TokenStore` tokens, races receive waits against an `async::sleep_for` heartbeat timer, applies the close-code/backoff reconnect policy, and returns one non-lifecycle `GatewayDispatch` per `next_dispatch()` resume. Slice 233 adds `qq::QqChannel` plus `normalize_gateway_dispatch(...)`; the receive-side trait adapter maps C2C/group message dispatches into `channel::InboundMessage` with QQ conversation ids, origin/capabilities, reply references, and mention-stripped group text, and slice 234 implements passive text replies over `ApiClient` (`c2c:` / `group:` endpoint selection, `content` / `msg_type:0` / `msg_id` / `msg_seq` body serialization, and `id` / `msg_id` / `message_id` receipt parsing). Slice 235 adds gated bootstrap registration through `channels[].kind == "qq"`: enabled `oran-bootstrap` builds own the QQ HTTP/token/API/gateway/channel stack through a private wrapper before registering it as a generic `Channel`; disabled builds skip/report QQ entries without linking this target. Round-trip acceptance remains the next QQ-port milestone | `oran-core`, `oran-async`, `oran-http`, `oran-channel` |
| `oran-channel-discord` | Discord adapter (optional, gated) | `oran-channel`, `oran-http` |
| `oran-channel-slack` | Slack adapter (optional, gated) | `oran-channel`, `oran-http` |
| `oran-channel-telegram` | Telegram adapter (optional, gated) | `oran-channel`, `oran-http` |
| `oran-channel-webhook` | generic webhook adapter | `oran-channel`, `oran-http` |
| `oran-desktop`       | Native desktop app (Slint, in-process). Slice 248 ships the always-built bridge surface (`desktop::gui_compiled()` build-config accessor, `test-desktop`) plus the gated `--desktop=y` Slint shell — a custom prebuilt-binary `slint` package, a `.slint`→C++ codegen `before_build` rule confined to the target, the `shell::run` skeleton window, and the `orangutan --desktop` launch (default builds print a "rebuild with `--desktop=y`" error). Slice 250 adds the always-built runtime bridge / view-model (`<oran/desktop/chat_bridge.hpp>`, all pure C++, no Slint): a `ChatViewModel` folding `UiUpdate`s into transcript state, a `DesktopEventSink : provider::EventSink` translating streamed deltas into `UiUpdate`s via a delivery hook, and a `ChatBridge` owning the bounded UI↔runtime queues (`async::Channel<std::string>` prompts, `async::Channel<UiUpdate>` updates) plus a per-turn `asio::cancellation_signal`, with overflow-drop accounting; `bootstrap::AgentPromptRunner` takes an optional injected `provider::EventSink*` so the desktop reuses the full loop runner. Slice 251 adds the always-built session driver `run_chat_session` (loops `next_prompt`→turn→stream until the input closes, taking a fresh per-turn `begin_turn()` cancellation slot so a stop ends one turn, not the session) and uses `async::Runtime::start()` to run the runtime alongside the Slint loop; `ChatBridge::close()` became input-only so a final in-flight turn's deltas stay drainable. Slice 251 also added the session driver `run_chat_session`. Slice 252 binds it together: the gated `--desktop=y` Slint chat UI (`ui/app_window.slint` — transcript + input + Send/Stop) driven by `shell::run` (a `slint::Timer` drains the bridge into a `ChatViewModel`), plus the `orangutan --desktop` launch in `oran-bootstrap` (`run_desktop`) that assembles runtime/provider (live `HttpProviderBackend`, else a scripted `FakeProvider`)/runner/bridge, `Runtime::start()`s, co-spawns `run_chat_session`, and opens the window. | `oran-core`, `oran-async`, `oran-provider`; planned `oran-agent`, `oran-orchestration` |
| `oran-cli`           | REPL / single-shot mode parser with deterministic no-runner shell plus slice-100 `PromptRunner` / `run_async` seam for caller-owned async prompt execution, slice-125 `CliOptions::interactive_repl` terminal stdin loop for provider-backed REPL prompts, slice-128 REPL slash-command handling (`/help`, `/exit`, `/quit`) before prompt-runner dispatch on both scripted and interactive paths, slice-95 `OperatorPromptSink` for terminal `permission_ask_rendered` approvals (scripted answers for tests/noninteractive drivers, asio stdin read for interactive answers), and (slice 123) `cli::StreamingPromptSink` — a `provider::EventSink` that renders answer/thinking deltas to an injectable `std::ostream` (default `std::cout`, flushed per delta for character-by-character output) plus a one-line `[tool: <name>]` marker per tool call, and reports `rendered_answer_text()` so the prompt runner can suppress the duplicate final-text print; planned line editor/history and additional slash-command targets once runtime command surfaces exist | currently `oran-core`, `oran-async`, `oran-hook`, `oran-provider`; planned `oran-orchestration` |
| `oran-bootstrap`     | Process entry, config discovery, runtime assembly, provider-route preflight, CLI/desktop/service modes, prompt-runner bridges, trace/audit helpers, configured channel registration/routing, and automation seed handoff. `RuntimeAssembly` owns permission/audit/workspace/trace/hooks/session/long-term-memory infrastructure plus diagnostic automation descriptors. Ordinary startup maps retention, cron, and triggered config into descriptors without mutating `automation.db`; `orangutan --serve` is the explicit long-lived owner that opens automation state, applies cron plus triggered seeds, drives `AutomationService::run(...)`, reaps the shared tool scheduler, starts configured channel ingress, and dispatches channel fan-in through bounded per-channel+conversation worker queues with idle eviction, structured worker metrics snapshots plus a deduplicated stderr metrics sink, and optional per-message deadline fallback while preserving triggered enqueue. | currently `oran-core`, `oran-async`, `oran-http`, `oran-io`, `oran-storage`, `oran-config`, `oran-permission`, `oran-hook`, `oran-memory`, `oran-automation`, `oran-channel`, `oran-skill`, `oran-tool`, `oran-provider`, `oran-agent`, `oran-cli`; optional `oran-channel-qq` only under `--channel_qq=y`; planned every public lib above |

Bootstrap trace-export sink note: slice 242 adds `--trace-export-post <url>`
beside stdout and `--trace-export-file <path>`. The POST sink lives in
`oran-bootstrap`, reuses the existing read-only trace export JSON Lines payload,
sends it through `oran-http::Client` as `application/x-ndjson`, suppresses
stdout, and treats non-2xx responses as IO errors with the status code.

Automation retention ownership note: slice 205 keeps automation state explicit
and caller-owned. Slice 192 added
`automation::AutomationRuntime::open(...)` as the explicit caller-owned state
handle above `storage::Pool`. It creates parent directories, opens and migrates
`automation.db`, exposes the migration report and repository, can explicitly
apply cron seed descriptors or run one caller-awaited cron service cycle, and
constructs retention services over that stable state. Slice 193 adds
`automation::MemoryRetentionLoop::run_once(...)` as a caller-started awaitable
above that service/runtime state. Slice 194 adds advisory job lifecycle
metadata from due retention ticks when the caller supplies a hook bus.
Slice 195 adds stored retention job leases and has the loop lease only due
execution. Slice 196 adds `automation::MemoryRetentionLoop::run(...)` as a
finite caller-owned policy over the leased step. Slice 197 adds pure cron
schedule evaluation, slice 198 adds durable cron job state, slice 199 adds
caller-driven cron scan/wait over that state, slice 200 adds caller-driven
cron due execution that advances state only after a supplied handler succeeds,
slice 201 adds finite caller-owned cron loop policy, and slice 202 adds
advisory cron lifecycle hook metadata. Slice 203 adds config-authored cron
schedule seeds and bootstrap mapping into `automation::UpsertCronJobRequest`
descriptors, and slice 219 makes those descriptors carry required
`agent_prompt`, without making bootstrap open or run automation state. Slice
220 adds prompt-runner handler factories that consume the same stored prompt
through caller-injected runner callbacks, still without making bootstrap open or
run automation state. Slice 204
adds `AutomationRuntime::apply_cron_job_seeds(...)` so a caller that already
opened automation state can upsert those descriptors explicitly. Slice 205 adds
`AutomationRuntime::run_cron_service_cycle(...)` so a caller can validate finite
service policy, apply supplied seeds, and await the existing cron loop in one
explicit startup cycle. Slice 223 adds
`AutomationRuntime::automation_service(...)` and move-only
`automation::AutomationService` as the first composed owner above buffered
triggered work plus one cron cycle, still without detached background
ownership. Slice 224 lets that same owner keep same-agent blocked triggered
descriptors in owner-local retry state for later explicit cycles, retry them
before newer queued work, and keep delayed retry lease/finish timing aligned to
the actual attempt time without broadening public queue-drain semantics. Slice
225 adds `AutomationService::run(...)` as the finite caller-owned loop policy
above that owner, repeating explicit cycles over caller-owned iteration and
retry-wait budgets while held blocked triggered work remains. Slice
206 adds cron run history rows for explicit
success/failure handler attempts without changing bootstrap ownership. Slice
208 adds typed cron run outcomes on those rows and classifies cancelled handler
errors as `aborted`, still without changing bootstrap ownership. Slice 209
adds repository-backed cron execution leases for explicit loop owners, and
slice 210 adds stored cron `agent_key` plus repository-backed cron agent leases
for the same explicit loop owners. Slice 211 adds triggered job descriptors,
slice 219 makes those descriptors carry required `agent_prompt`, and
caller-driven triggered intake runs over the same caller-owned automation runtime
state. Slice 212 adds triggered run rows plus caller-supplied triggered
handler execution over that intake path, slice 213 adds advisory triggered job
lifecycle metadata around those explicit handler attempts, and slice 214 adds
triggered agent leases for explicit triggered handlers.
`oran-bootstrap` maps configured retention policy into a job descriptor and
configured cron/triggered jobs into repository seed descriptors. Slice 257 adds
`AutomationRuntime::apply_triggered_job_seeds(...)` so a caller that already
opened automation state can persist triggered descriptors explicitly. Ordinary
startup does not automatically open `automation.db`, apply those seeds, start
timers, start a long-running loop, intake trigger events, or fire agents.

Automation cron ownership note: slice 198 adds repository-owned
`automation_cron_jobs` rows plus public upsert/load/list/mark-fired APIs for
durable cron schedule and last-fired state; slice 219 makes the stored row also
carry required prompt input. Slice 199 adds a read-only
`CronService::tick(...)` scan and a one-step `CronLoop::run_once(...)` wait over
stored rows. Slice 200 adds `CronService::execute_due(...)` so callers can run a
supplied payload handler and mark the due fire only after success. Slice 201
adds `CronLoop::run(...)` so callers can catch up due fires and wait within a
finite budget without starting a daemon. Slice 202 adds advisory cron lifecycle
hook metadata around handler execution. Slice 203 adds typed
`automation.cron.jobs[]` parsing and bootstrap seed mapping. Slice 204 adds
explicit seed application through caller-owned `AutomationRuntime`. Slice 205
adds one caller-awaited cron service cycle that validates finite service policy,
applies seeds, and awaits the existing finite cron loop. Slice 206 adds
repository-backed cron run history for explicit due-execution attempts and
keeps not-due scans free of run rows. Slice 207 adds cooperative stop
policy for finite cron loops and service cycles. Slice 208 adds typed
`success` / `failure` / `aborted` cron run outcomes and maps
`ErrorKind::cancelled` handler results to `aborted`, while failed and aborted
handlers remain due for explicit retry. Slice 209 adds repository-backed cron
execution leases for explicit loop owners. Slice 210 adds stored cron
`agent_key` plus repository-backed cron agent leases for the same explicit loop
owners, slice 219 adds required cron `agent_prompt` persistence, slice 220
adds `make_cron_prompt_handler(...)` for injected prompt execution through the
same explicit service/loop paths, and slice 221 adds bootstrap-owned bridge
wiring into configured-route `AgentPromptRunner` without moving runtime or DB
ownership into bootstrap. Slice 222 adds caller-owned notifier callbacks plus
output-carrying cron attempt results after durable run/state advancement,
without choosing concrete cli/channel/desktop routing, and slice 223 adds the
composed `AutomationService` owner above buffered triggered work plus one cron
cycle. Detached process timers, queueing, concrete notifier routing, queue
hold/drop policy for blocked agent leases, and agent firing remain downstream.

Automation triggered ownership note: slice 211 adds repository-owned
`automation_triggered_jobs` rows plus public upsert/load/list-by-trigger APIs
for durable triggered descriptors; slice 219 makes those descriptors carry
required prompt input, slice 220 adds `make_triggered_prompt_handler(...)`
for injected prompt execution through the same explicit service/queue paths,
and slice 221 adds bootstrap-owned bridge wiring into configured-route
`AgentPromptRunner`. Slice 222 adds the same caller-owned notifier callback
surface plus output-carrying triggered attempt results after durable run-row
recording, while concrete cli/channel/desktop routing remains outside
`oran-automation`.
`TriggeredService::intake(...)` matches a
caller-supplied external `trigger_key` to stored descriptors and preserves the
intake timestamp in the result. Slice 212 adds repository-owned
`automation_triggered_runs` rows plus public record/list APIs for durable
triggered handler attempts. `TriggeredService::execute(...)` reuses intake,
invokes a caller-supplied handler once per matched descriptor, records
`success`, `failure`, or `aborted` outcomes, and continues through per-attempt
handler errors. Slice 213 lets callers pass `TriggeredServiceOptions::hooks` so
that execution publishes advisory `job_started`, `job_failed`, and
`job_finished` metadata around each handler attempt. Slice 214 adds
repository-owned `automation_triggered_agent_leases` rows plus public
acquire/release APIs for optional same-agent explicit triggered execution
ownership. When callers supply `TriggeredExecuteRequest::lease_owner_key`,
`TriggeredService::execute(...)` acquires the stored triggered job `agent_key`
before handler work, rejects active conflicts before handler/run/hook work, and
releases after durable success/failure outcomes. Slice 215 adds a caller-owned
`TriggeredQueue` over triggered intake so runtime owners can buffer matched
descriptors in bounded process-local state, explicitly receive queued jobs, and
observe `drop_newest` overflow through advisory `job_dropped` metadata. Slice
216 adds `TriggeredService::execute_one(...)` plus
`TriggeredQueue::drain_once(...)` so callers can execute exactly one queued
descriptor without re-intaking by trigger key or consuming later queued work. It
does not notify channels, call agents, or start a detached drain loop. Slice
217 adds drain-time `drop_on_conflict` for active triggered-agent lease
conflicts, slice 218 adds finite available-batch draining, slice 223 adds the
composed `AutomationService` owner that drains buffered triggered work before
one cron cycle, and slice 224 lets that owner hold same-agent blocked
descriptors for later explicit retry cycles while public queue drains remain
drop-only. Slice 225 adds the finite caller-owned service-loop ownership above
`AutomationService`. Slice 257 adds config-authored triggered descriptor seed
mapping and explicit persistence; slice 258 lets the configured-channel `--serve`
loop call `AutomationService::enqueue_triggered(...)` with `channel:<channel_id>`
keys before it sends the direct channel reply. Webhook/non-chat trigger producers
remain downstream.

Bootstrap service ownership note: `orangutan --serve` remains the explicit
long-lived owner for automation state. As of slice 258 it opens
`<workspace>/.orangutan/automation.db` when either `automation.cron.jobs[]` or
`automation.triggered.jobs[]` is configured, applies cron seeds first and
triggered descriptor seeds second, then drives `AutomationService::run(...)`
over prompt-backed handlers; when configured channels are also active, it can
enqueue matching triggered work from channel ingress. Ordinary startup still
only stores mapped descriptors on `RuntimeAssembly`; it does not open or mutate
automation state.

**Binaries** built on top:

| Binary             | Description                                                  |
| ------------------ | ------------------------------------------------------------ |
| `orangutan`        | Default: CLI REPL or single-shot, optional `--desktop` and channel modes. |
| `orangutan-server` | Daemon mode: channels + automation, no terminal UI.    |
| `orangutan-bench`  | Standalone runner that executes the `bench/<lib>/...` buckets and emits JSON. |

## Boundary Rules

- **One-way dependencies.** Each library lists what it is *allowed* to depend on in the
  table above. CI enforces this with `scripts/check-deps.sh`.
- **No globals.** The bootstrap layer owns lifetimes; everything else takes context
  objects by reference. See `docs/design-docs/module-boundaries.md`.
- **No back-channels.** If a feature needs to influence something outside its layer, it
  surfaces a hook or a callback. See `docs/design-docs/permissions-and-hooks.md`.
- **No `friend` across libraries.** Friendship stays in the same library, ideally the
  same TU.
- **Public headers under `include/oran/<lib>/`** are forward-declaration heavy; full
  includes live in `src/<lib>/`. This is enforced by `docs/rules/module-and-pch.md`.

## Data Flow (Single Turn)

```
1.  Inbound message arrives via cli / desktop / channel / automation.
2.  oran-bootstrap routes it to the right Agent (per agent_key + identity).
3.  oran-agent::run(prompt):
      a. hook bus  → AgentLifecycle::iteration_start
      b. oran-prompt → build cached prompt (memory section computed ONCE)
      c. oran-provider → send (transport → protocol → execution)
      d. parse response → for each tool_use:
           - oran-permission → check
           - hook bus → ToolLifecycle::before
           - oran-tool → dispatch
           - hook bus → ToolLifecycle::after
      e. append history → loop until stop_reason = end_turn or MAX_ITERATIONS
      f. hook bus → AgentLifecycle::iteration_end / final_response
4.  oran-storage persists conversation.
5.  oran-memory distills new long-term facts (async, off the critical path).
```

## Async Topology

- **One executor** (`asio::io_context` wrapped in `oran::async::Runtime`) drives all I/O.
- **Worker pool** is asio's thread pool; size comes from `config.runtime.workers`.
  The current config default is `4`; hardware-aware bootstrap defaulting can replace
  that when `oran-bootstrap` owns runtime assembly.
- Long-running CPU work (memory distillation, prompt assembly when very large) runs on
  `oran::async::cpu_pool`, a separate fixed-size pool, surfaced as `asio::any_io_executor`.
- See `docs/design-docs/async-model.md` for the full topology.

## State Layer

- **SQLite** for sessions, memory, automation jobs, hook audit logs.
- **One database file per concern**: `sessions.db`, `memory.db`, `automation.db`,
  `audit.db`. Crash isolation; smaller WAL contention.
- **WAL mode** by default; one writer connection per DB, read-pool for queries.
- **Migrations** versioned and applied at startup; see
  `docs/design-docs/secrets-and-state.md`.

## Configuration

- `config.example.json` is checked in and load-tested by `oran-config`.
- `oran-bootstrap` now accepts `--config <path>` / `--config=<path>`. Explicit paths
  are required to load successfully; without `--config`, bootstrap loads
  `<workspace>/.orangutan/config.json` when present and uses built-in config defaults
  when it is absent in this early runtime slice. After config loading, bootstrap
  preflights the configured `default` provider route when routes exist, constructs
  the HTTP-backed provider backend, then hands CLI mode flags such as `--prompt`
  to `oran-cli` through `cli::run_async` with `AgentPromptRunner`; no-prompt
  configured-route runs enable `CliOptions::interactive_repl` and read terminal
  prompts until an empty line or EOF. Built-in empty defaults still use the
  deterministic no-runner shell, and tests also exercise the
  runner seam with `provider::FakeProvider`.
- The current typed surface covers `strict_config`, `runtime` (including
  `tool_output` caps and `prompt.active_tools`), `profiles` (including
  optional `protocol`), `routes`,
  `session`, `desktop`, `trace`, `hooks.timeout_ms`, `permissions`,
  `agents.<name>.permissions`, and `agents.<name>.skills_enabled`; planned
  sections such as channels, teams,
  hook sinks/bindings, memory, automation, and the remaining
  `agents.<name>.*` fields (provider/model override, prompts, hook
  bindings) are accepted as recognized fields until their typed models land.
- `${VAR}` and `${VAR:-default}` substitutions run on string values at load time.
- Secret encryption, generated JSON Schema, and rotation remain planned follow-up
  slices. See `docs/design-docs/secrets-and-state.md`.

## To Fill In As The Project Matures

This file should grow with the project but stay scannable. The expected next edits are:

- Datapath diagrams per channel adapter once the first three adapters land.
- Identity / scope diagram once memory tiers ship.
- Observability stack diagram (logs / metrics / traces) once shipping.
- Deployment topology once a real runtime target exists.
