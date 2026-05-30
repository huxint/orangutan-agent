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
(CLI, web, channels, automation), backed by **shared storage and policy**. Everything is
asynchronous on a single executor; everything that crosses a process boundary goes through
a transport trait; everything observable goes through a hook surface.

```
                           ┌─────────────────────────────────────┐
  CLI REPL ────────────▶   │                                     │
  CLI single-shot ────▶    │       INTERFACE LAYER               │
  Web (HTTP/SSE) ──────▶   │  cli  •  web  •  channel  •  cron   │
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
> `ReadTextSingleflightStats`,
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
> optional `protocol`)/`routes`/`session`/`web`/`trace`/`hooks.timeout_ms` plus the
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
> Anthropic Messages and OpenAI Responses request JSON that also carries
> structured tool-result bytes; slice 108 adds
> `provider::decode_protocol_response(body_json, target)`, the paired offline
> response decoder for text/thinking/tool-use blocks, usage counters, model ids,
> and stop reasons; slice 109 adds
> `provider::ProtocolTransportAdapterFactory`, which composes those mappers over
> an injected body-response `ProtocolTransport` for Anthropic/OpenAI systems;
> bootstrap now owns the first concrete `http::Client`-backed construction seam,
> while SSE remains planned),
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
> filling usage counters for `file.write`, `file.edit`, and
> `file.delete` while leaving `data_json` empty; slice 62 migrates
> `file.read` by keeping the text header/body fallback while adding
> `data_json` (`kind`, `path`, requested `text`, `fingerprint`,
> `start_line`, `end_line`, `returned_bytes`, `truncated`) and usage
> counters (`bytes_read`, `files_touched`, `truncated`); slice 63
> migrates `file.search` by keeping the existing
> `path:line:text` text rendering and trailing truncation summary,
> filling `data_json` with `kind`, `path`, `pattern`, `regex`,
> `matches[]` (`{path, line_number, text}` per match), `match_count`,
> `truncated`, `truncation_reason` (null / `matches` / `bytes`),
> `files_scanned`, and `bytes_read`, and filling usage counters
> `bytes_read` (cumulative scanned file bytes), `files_touched`
> (non-binary scanned files), `match_count` (post-truncation), and the
> `truncated` cap flag; slice 64 migrates `directory.list` by keeping
> the existing `<path>:<kind>:<size>` text rendering, filling `data_json`
> with `kind`, `path`, `include_hidden`, `max_entries`, `entry_count`,
> and an `entries[]` array of `{name, path, kind, size_bytes}` (with
> JSON null for non-regular `size_bytes`), and filling usage
> `files_touched=1` plus `match_count=entry_count`;
> `entries_` uses
> transparent string hashing so lookup by `std::string_view` in
> `remove`, `find`, and `dispatch` does not allocate a temporary key;
> `tool::Workspace` (slice 37) canonicalises tool-layer paths without
> exposing `<filesystem>` in the public header, and
> `DispatchContext::workspace` is the interim non-owning seam file
> built-ins consume until `tool::Runtime::workspace()` lands; slice 38
> adds `file.write` / `file.edit` / `file.delete` adoption through that
> seam, slice 39 adds `file.search` through `resolve_list`, and slice 40
> adds `directory.list` through `resolve_list` so every filesystem
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
> straight to `ApprovalBroker::approve`; built-ins `file.read`
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
> `file.write` (slice 18,
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
> `file.edit`
> (slice 19, `tool::register_file_edit`, capability `edit_file`,
> input `{path, old_string, new_string, replace_all?, max_bytes?, expected_version?}` —
> `conflict` if `old_string` is not unique unless `replace_all` is
> set, `not_found` if `old_string` is absent, and the read + final
> replacement output are capped by `max_bytes`; slice 38 resolves
> through `tool::Workspace::resolve_write` when supplied; slice 46
> guards the edit behind the same optional `expected_version`
> contract as `file.write`; slice 61 fills
> `Output::usage.bytes_read`, `bytes_written`, `files_touched`,
> and `match_count` on success), `file.search`
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
> `directory.list` (slice 29, `tool::register_directory_list`,
> capability `list_directory` (new in `core::Capability`), input
> `{path, include_hidden?, max_entries?}` — single-level
> enumeration through `oran-io::list_directory`; renders one
> `<path>:<kind>:<size_bytes or '-'>` line per entry sorted by
> path, the literal text `no entries` when the directory is empty
> after the hidden filter, and propagates the `io: directory
> entry limit exceeded` error verbatim when `max_entries` is
> exceeded — raise the cap and retry; slice 40 resolves through
> `tool::Workspace::resolve_list` when supplied), and `file.delete`
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
> bind / unbind / `publish_advisory` — the bus iterates sinks
> in subscription order, makes per-sink payload copies, clears raw
> `ToolAfterPayload::data_json` for non-trusted-local sinks, captures
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
> owns `prompt::PromotionState`, observes successful `tool.search`
> structured output, promotes deferred matches into the next prompt
> snapshot, ignores failed/non-search outputs, rejects malformed
> successful structured data without partial mutation, and keeps
> `nlohmann_json` isolated in `src/oran-agent/session_state.cpp`;
> `agent::Loop` now builds the rendered prompt, maps cache hints and active
> tools into `provider::Request`, drives a supplied `provider::System`,
> and, when supplied a `tool::Registry` plus `tool::DispatchContext`,
> sequentially dispatches `tool_use` blocks, appends ordered tool results,
> re-enters the provider until a terminal turn or iteration cap, tags
> parent-cancelled provider/tool awaits with `cancellation_phase`, and writes
> cancelled trace rows when trace is configured; slice 79
> threads `RunTurnInputs::turn_id` into direct dispatch audit rows when
> tracing is enabled, slice 85 generates that id when a trace writer is
> configured and callers leave it unset, clears/restores reusable context ids
> when tracing is explicitly disabled, writes one body-free `trace_turns` row
> before returning terminal-success responses, and writes provider/tool cancelled rows
> before returning parent-cancelled errors; slice 96 also refreshes
> `DispatchContext::now` around direct dispatch so broker-backed
> `permission_ask_rendered` prompts use a real per-call timestamp)
> are implemented.
> Rows whose purpose still says "planned" will land per `docs/exec-plans/` as
> future slices are scheduled. The build system, PCH, tests bucket, and bench
> bucket conventions are live; see the history entries under
> `docs/histories/2026-05/`.

| Library              | Purpose                                         | Depends on (allowed)                          |
| -------------------- | ----------------------------------------------- | --------------------------------------------- |
| `oran-core`          | `Result<T>`, `Error`, `Time` + ISO-8601 UTC helpers, `Role`, `StopReason`, `Content` variant, `Message`, `ToolDef` (with `required_capabilities`, `deferred`, and `category`), `core::str` UTF-8 helpers, `Capability` vocabulary (20 enumerators, slice 29 adds `list_directory`), `core::TurnId` (slice 79 shared 16-byte trace/audit correlation id), and (slice 44) the generic `core::BoundedCache<Key, Value>` primitive (LRU on access, insert-based TTL, byte-budget eviction, explicit `erase_if` invalidation, customizable byte-size functor via template parameter, `Stats` accessor exposed for the future `oran-log`) | stdlib only |
| `oran-async`         | asio `Runtime`, `Awaitable<T>`, bounded `Channel<T>`, cancel-aware `sleep_for`; mailbox policy lands in orchestration | `oran-core`, asio |
| `oran-log`           | spdlog shim + secret redaction; thread-local context | `oran-core`, spdlog/fmt |
| `oran-io`            | file/directory IO MVP — `read_text_file`, `write_text_file`, `list_directory`, `delete_file` (slice 30, regular-file only), (slice 42) `io::FileFingerprint` + `io::compute_file_fingerprint`, (slice 43) the range-aware `io::read_text_file_ranged` returning `ReadTextResult { text, fingerprint, start_line, end_line, returned_bytes, truncated }` with `FileRange { LineSpan | ByteSpan }` input validation, mid-read fingerprint capture (size/mtime drift -> retry once for whole-file reads under 64 KiB, surface `Error::conflict` for larger or ranged reads), dual-end UTF-8 code-point boundary alignment for byte ranges, (slice 50) a bounded `core::BoundedCache`-backed line-offset index for line ranges in files larger than 256 KiB, (slice 52) a bounded file-view cache for successful `ReadTextResult` payloads keyed by canonical path + range + max-bytes + cheap fingerprint, (slice 54) public `ReadTextFileCacheStats` for those two caches, (slice 57) public `invalidate_read_text_file_ranged_cache(path)` for path-scoped invalidation, (slice 58) `watch_read_text_file_ranged_cache(executor, root, options)` for cancel-aware Linux/inotify external-edit cache invalidation with aggregate `ReadTextFileWatchStats`, and (slice 53) a bounded 64-entry singleflight table for concurrent cold `read_text_file_ranged` calls with public `ReadTextSingleflightStats`; both caches invalidate the affected canonical path after successful in-process writes/deletes / watcher events and file-view hits revalidate with `stat` before returning; planned glob, pipe, subprocess, signal, content hashing, and bootstrap/config startup wiring for long-lived watchers | `oran-core`, `oran-async` |
| `oran-http`          | libcurl-backed HTTP client: `<oran/http.hpp>` exports stdlib-shaped `Header`, `BodyRequest`, `BodyResponse`, `SseEvent`, and pimpl-backed `http::Client` with `send` (collect one body) and (slice 121) `send_streaming(BodyRequest, SseEventCallback)` for Server-Sent Events: an internal incremental `text/event-stream` parser (`src/oran-http/_impl/sse_parser.hpp`) runs inside the libcurl write callback on the blocking executor, and each decoded `SseEvent` is `asio::post`-ed to the caller's coroutine executor before the sink runs (the decoder/sink never run on the curl thread); a 2xx `text/event-stream` response resolves with status + headers and an empty body, any other response is collected into the body for the caller to decode, and the existing 50 ms curl poll surfaces mid-stream cancellation; callers provide the executor used for blocking curl work, curl handles remain private, and server/router surfaces remain planned | `oran-core`, `oran-async`, libcurl |
| `oran-storage`       | SQLite expected-only connection/statement core with text/int/double/null plus BLOB bind/read support, migration runner with SQL-file loading **and compile-time-embedded built-in migrations** (`built_in_audit_migrations()` / `built_in_session_migrations()` / `built_in_trace_migrations()` reach the SQL via C++26 `#embed`; trace is audit DB migration version 2, audit parent-turn correlation is version 3, and audit event-kind discrimination is version 4), async writer/reader `Pool` with per-slot `StatementCache`, standalone per-connection `StatementCache`, `SessionRepository` (typed `core::Role` at the API boundary), `AuditRepository` (typed audit-event append/update-metadata/list/count over `audit_events`; slice 67 adds same-row metadata replacement for post-result usage enrichment, slice 79 adds nullable `parent_turn_id` BLOB matching so usage enrichment cannot cross same-tool traced turns, slice 88 adds the operator-level `list_events_for_turn(TurnId, limit)` read that joins by `parent_turn_id` ordered `id ASC` so the spec-0017 multi-tool dispatch order survives the trace/audit join, and slice 93 adds `event_kind` on append/update/list records so permission-decision rows and `hook_publish` rows can share the same table without metadata-update clobbering), and `TraceRepository` (typed spec-0018 `trace_turns` append/get/list/count over `core::TurnId` / BLOB turn/session ids, prompt/cache hashes, usage rollups, optional cancellation phase, and redacted context bytes; slice 80 adds the first `oran-agent` terminal-success writer, slice 82 makes the loop skip that writer when tracing is explicitly disabled, slice 83 adds provider/tool cancellation-row writes, slice 84 adds provider/loop-boundary error-row writes, and slice 85 lets the loop generate missing trace turn ids); planned memory/automation repositories | `oran-core`, `oran-async`, sqlite3 |
| `oran-config`        | JSON config loader with typed runtime/profile/route/session/web/trace/hooks fields, including `runtime.tool_output.max_text_bytes` / `max_data_bytes` defaults for structured tool-output caps, `runtime.prompt.active_tools` as either `"defaults"` or an explicit active-tool allowlist consumed by `oran-prompt`, slice-102 optional `profiles.<name>.protocol` strings for provider route resolution, slice-81 `trace.enabled` / `trace.store_raw_bodies` / `trace.retention_days` defaults for spec-0018 trace policy, and slice-92 `hooks.timeout_ms` for the blocking hook deadline, env substitution, the typed `permissions` + `agents.<name>.permissions` overlay surface (layer-2/3 data of the three-layer rule merge), and (slice 41) `permissions.workspace.extra_{read,write}_roots` parsed onto `WorkspacePermissionsConfig` for the bootstrap-owned `tool::Workspace`; planned schema + secret-protected fields plus typed hook sink/binding models | `oran-core`, `oran-storage` |
| `oran-permission`    | foundation rule evaluator: `Verdict`, `Mode`, `Rule`, `RuleSet`, `Decision`, `*`-glob tool matching, capability-aware gating (`Rule::capability` of `core::Capability`), the `Defaults::for_mode` baseline factory, the three-layer `materialize(Mode, global, per_agent)` merge that concatenates defaults + global config + per-agent overlay, the `ApprovalSecret` / `ApprovalAuthority` / `ApprovalToken` / `ApprovalBroker` ask-flow surface (including the slice-56 64-live-grants-per-identity cap), and the `AuditEvent` / `AuditMetadataUpdate` / `AuditSink` / `StorageAuditSink` audit pipeline (`record` plus slice-67 same-row metadata update; slice 79 carries optional `parent_turn_id` through record/update paths; slice 91 adds `AuditOutcome::blocked_by_hook` and `AuditOutcome::rewritten` for blocking hook decisions; slice 93 carries `event_kind` through record/update paths so `hook_publish` rows cannot be mistaken for ordinary permission decisions); planned re2 input regex extensions and bootstrap wiring | `oran-core`, `oran-config`, `oran-storage`, `oran-async` |
| `oran-skill`         | skill loader, skill catalog | `oran-core`, `oran-io` |
| `oran-tool`          | tool registry (`Registry::add` validates declarations and `input_schema_json` before insertion; `Registry::entries_` uses transparent string lookup for `std::string_view` names), `tool::CatalogRenderer` (deterministic prompt-facing catalog renderer: sorted full-schema active blocks, deferred name/description rows, and aggregate stats for a bounded 256-entry rendered-block cache keyed by rendered-block fields; consumed by `oran-prompt` for section 2/3 bytes), `tool::Output` / `ToolUsage` / `Attachment` (required text fallback, optional serialized `data_json`, metadata-only attachments, usage counters and cap flags; `Output::text_only` preserves the v1 text path) plus `OutputCapOptions` / `OutputCapReport` / `apply_output_caps` for spec-0014 text/data byte caps, `tool::Workspace` path resolver (`resolve_read` / `resolve_write` / `resolve_delete` / `resolve_list`, with canonical roots, traversal rejection, symlink policy, and extra read/write roots), registry-boundary workspace pre-resolution for known filesystem built-ins (`DispatchContext::resolved_path`, redacted `metadata_json.path_resolution`, resolver failures audited before handler execution / ask replay), transient dispatch registry context (`DispatchContext::registry` is set to the currently dispatching registry and restored on exit so metadata tools can inspect the live catalog without self-capturing a movable registry), dispatch through `permission::RuleSet` + `permission::AuditSink` with the slice-21 `(ApprovalBroker*, ApprovalToken*)` mediation of `Verdict::ask` (audit outcome promoted to `approved` on broker.check OK, `rejected` on broker rejection — `expired` / `tool_mismatch` / `identity_mismatch` / `input_mismatch` / `mac_mismatch` / `no_grant` / `replay_exhausted` reason forwarded; the short-circuit `approval_required` path carries `decision_reason` / `replay_max` / `approval_ttl_seconds` in the error context) and the slice-22+25+60+65+66+67+79+91 `hook::Bus*` / output-boundary / audit-metadata tap (publishes blocking `tool_before` before workspace resolution and permission evaluation, consuming veto / rewrite / require_approval decisions and writing `metadata_json.hook_decisions`; publishes advisory `tool_dispatched` on `allow` / ask-approved paths, `tool_error` on error exits, and `tool_after` at every exit; successful dispatch first applies `DispatchContext::output_caps`, then best-effort enriches the same audit row's `metadata_json.usage`, with optional `DispatchContext::parent_turn_id` carried into both the decision row and metadata update for trace correlation, and `tool_after` receives capped `Output::text`, `Output::usage` metrics, and raw `Output::data_json` when retained, with the bus redacting `data_json` for non-trusted-local sinks), built-ins `file.read` (range-aware schema `{path, start_line?, line_count?, offset_bytes?, length_bytes?, max_bytes?, if_version?}` through `io::read_text_file_ranged`; `if_version` can short-circuit to `Error::not_modified`; success keeps the text header/body fallback, fills `data_json` with requested text plus range/fingerprint metadata, and fills usage bytes-read + file-count + truncation flag), `file.write` (`{path, content, mode?, create_parents?, max_bytes?, expected_version?}`; 16 MiB cap; stale `expected_version` fails with `reason=stale_fingerprint`; success fills usage bytes-written + file-count), `file.edit` (`{path, old_string, new_string, replace_all?, max_bytes?, expected_version?}`; stale token guard; success fills usage bytes-read/written + file-count + match-count), `file.search` (`{path, pattern, max_matches?, include_hidden?, regex?, max_output_bytes?, respect_ignore?}`; regex compile cache, output cap, built-in skips, `.gitignore` / `.ignore` subset; success keeps the `path:line:text` text rendering and fills `data_json` with kind, path, pattern, regex, matches[], match_count, truncated, truncation_reason, files_scanned, bytes_read plus usage bytes-read + file-count + match-count + truncated flag), `directory.list` (`{path, include_hidden?, max_entries?}`; success keeps the `<path>:<kind>:<size>` text rendering and fills `data_json` with kind, path, include_hidden, max_entries, entry_count, and an `entries[]` array of `{name, path, kind, size_bytes}` (size_bytes null for non-regular) plus usage `files_touched=1` and `match_count=entry_count`), `file.delete` (`{path}` regular-file-only; success fills usage bytes-written zero + file-count), and `tool.search` (`{name?, category?, capability?}` metadata lookup; no required capability; success returns text plus `data_json` with query, match_count, and matched tool definitions including nested `input_schema`, required capabilities, deferred flag, and nullable category, with usage `match_count`; `agent::SessionState` consumes successful payloads for deferred-tool prompt promotion); slice 107 provider request serialization consumes `data_json` for Anthropic/OpenAI tool-result blocks, while `tool::Runtime` and the rest of the built-in catalog remain planned | `oran-core`, `oran-async`, `oran-permission`, `oran-hook`, `oran-io` |
| `oran-hook`          | hook bus + sink kinds: `Event` enum (41 lifecycle events), `Sink` abstract interface with `SinkKind::default_` / `SinkKind::trusted_local` plus a virtual `handle_blocking(Event, Payload) -> Awaitable<Result<HookDecision>>` defaulting to `proceed` (body in `src/oran-hook/sink.cpp`), `BusOptions { blocking_timeout }`, `Bus` with `bind` / `unbind` / `publish_advisory` (advisory contract — sinks observe, never veto; per-sink payload copies redact `ToolAfterPayload::data_json` for non-trusted-local sinks; sink exceptions are caught and surfaced as `Error::internal` in `PublishOutcome`) plus `publish_blocking<E>` (constrained by `requires HasBlockingDecision<E>`; walks subscribed sinks in subscription order, records `HookDecision::trace` for every consulted sink, short-circuits at the first non-`proceed` decision, applies the same redaction the advisory path uses, converts sink `core::Result` errors / thrown exceptions into a veto with `reason="hook_error: <message> [sink=<id>]"`, and races each sink against `blocking_timeout`, synthesizing `reason=hook_timeout` with `HookDecisionTrace::elapsed` on expiry; with no sinks subscribed or every sink returning `proceed`, yields a proceed decision), `HookDecision { kind, reason, optional<string> rewritten_input_json, optional<core::Time> approval_expires_at, vector<HookDecisionTrace> trace }` and `HookDecisionKind { proceed, veto, rewrite, require_approval }` in `<oran/hook/decision.hpp>`, `EventTraits<E>` (empty primary template; explicit specialisations setting `Decision = HookDecision` for the spec-0015 v1 whitelist `tool_before` / `permission_ask_rendered` / `memory_write_before`) plus the `HasBlockingDecision<E>` concept in `<oran/hook/event_traits.hpp>`, and `InProcessSink` (std::function-backed `Callback` + optional `BlockingCallback` via `set_blocking_handler`); planned `ShellSink` / `WebhookSink` / `LuaSink` (`sol2` feature-gated) | `oran-core`, `oran-async` |
| `oran-memory`        | working / session / long-term / shared memory | `oran-core`, `oran-storage` |
| `oran-provider`      | provider-domain + fake-provider + route-resolver + execution-runtime slice: `<oran/provider.hpp>` exports the slice-73 cache-mapping shapes (`Request`, `Response`, `Usage`, `RetryPolicy`, `PromptCacheHints`, `PromptCacheOptions`, `make_prompt_cache_hints(RenderedPrompt, options)`) plus the slice-74 system surface (`ProtocolKind`, `ModelTarget`, `Route`, abstract `provider::EventSink` with default no-op text/thinking/tool delta callbacks plus terminal `on_done`, abstract `provider::System::send(Request, Route, EventSink*) const`, and `provider::FakeProvider` — the first concrete `System`) plus slice-97 `provider::execution::Runtime`, a `provider::System` decorator over a backend `System` that consumes `Request::retry.max_attempts` / `initial_backoff`, retries retryable `network` / `rate_limit` / `timeout` / `upstream` errors on the same target, tries `Route::fallbacks` in order after primary exhaustion, returns immediately for non-retryable errors and cancellations, sleeps backoff through cancel-aware `async::sleep_for`, suppresses retry/fallback after visible stream output to avoid duplicate caller-rendered bytes, and fills missing `Response::model_used` with the selected target model, plus slice-98/102/103 route resolution, slice-104 adapter planning, slice-105 credential resolution, slice-106 adapter factory dispatch, and slices 107-109 protocol request/response/transport mapping: `provider::resolve_route(Config, route_name)` still resolves config `profiles` / `routes` into `provider::Route`, preserves fallback order, prefers explicit `profiles.<name>.protocol` exact `ProtocolKind` spellings when present, otherwise maps provider aliases, and returns `Error::config` for missing profile references, unknown provider spellings, or unknown explicit protocol spellings; `provider::resolve_route_profiles(Config, route_name)` returns `provider::RouteProfileResolution` with the same primary/fallback `ModelTarget`s plus profile endpoint metadata (`provider`, `base_url`, `api_key_env`) and can derive the old loop-facing `Route` via `RouteProfileResolution::route()` for existing execution paths; `provider::make_adapter_construction_plan(resolution)` returns an offline `AdapterConstructionPlan` that keeps each resolved profile target beside its protocol adapter-family name, derives the old loop-facing `Route`, and validates non-empty provider/model/base-url/API-key-env metadata plus `http://` / `https://` endpoint schemes without reading secrets, constructing transports, or allocating concrete adapters; `provider::resolve_adapter_credentials(plan)` is the explicit environment-secret boundary for factories, returning `AdapterCredentialBundle` with in-memory API keys beside plan targets, deriving the same route, reporting missing/empty key env vars as `ErrorKind::auth`, and keeping error context to non-secret identifiers; `provider::make_adapter_system(credentials, factories)` consumes that bundle plus caller-registered `ProtocolAdapterFactory` bindings, validates missing/null/duplicate bindings and duplicate route profiles as config errors, and returns a profile-routed `System` that forwards one selected target per call to the matching backend; `provider::make_protocol_request(request, target)` serializes Anthropic Messages and OpenAI Responses request JSON bytes, validates opaque tool schemas/tool inputs/structured tool-result JSON privately in the `.cpp`, maps `ToolResultContent::data_json` into the structured tool-result channel when present, preserves text-only fallback, and rejects unsupported protocol families as config errors; `provider::decode_protocol_response(body_json, target)` decodes Anthropic Messages and OpenAI Responses response JSON bytes into `provider::Response`, including text, thinking/reasoning summaries, tool-use blocks, usage counters, model ids, and stop reasons while rejecting malformed response shapes as parsing errors; `provider::ProtocolTransportAdapterFactory` builds non-streaming Anthropic/OpenAI Responses `System` backends over an injected `ProtocolTransport`, injects provider API-key headers, maps HTTP status classes into provider error categories, and rejects selected route profile/model/protocol mismatches before sending; the cache mapper validates the single prompt breakpoint and maps sections 1-6 into adapter-facing `(id, content_hash, cache_version)` keys while excluding the conversation tail; the fake provider drives a `std::vector<ScriptedTurn>` of `{response | deltas | error, latency}`, opens/extends `TextContent` / `ThinkingContent` / `ToolUseContent` blocks from `StreamDelta`s, fans the same deltas through the supplied `EventSink`, awaits scripted latency via `async::sleep_for` so parent cancellation interrupts the wait, serialises concurrent `send` calls through a `mutable std::mutex` so the `const`-qualified contract holds, and returns `Error::internal` once the plan is exhausted or a scripted turn carries no body; slice 111's `bootstrap::HttpProviderBackend` supplies the first `http::Client`-backed `ProtocolTransport` adapter outside `oran-provider`, and slice 112 consumes it from configured-route `bootstrap::run`; SSE transport, provider hooks, usage/cost rollups, and real vendor cache-control mapping remain planned | currently `oran-core`, `oran-async`, `oran-config`, `oran-prompt`; HTTP adapter wiring stays outside `oran-provider` |
| `oran-prompt`        | prompt builder + promotion state: `BuilderInputs`, `BuilderOptions`, `CacheSection`, `RenderedPrompt`, and `PromotionState`; assembles the seven prompt-design sections, consumes `config::PromptActiveToolsConfig`, delegates section 2/3 catalog bytes to `tool::CatalogRenderer`, consumes sorted promoted-tool snapshots to move deferred tools into the next active catalog, computes stable section hashes, a cache-versioned prefix hash over sections 1-6, prefix byte count, and one breakpoint before the conversation tail; `PromotionState` is explicit-time, session-owned, capped at 16 names with a 24-hour TTL, and exposes aggregate stats without prompt byte drift; planned real skill/memory renderers | `oran-core`, `oran-async`, `oran-config`, `oran-tool` |
| `oran-agent`         | first loop surfaces: `agent::SessionState` owns `prompt::PromotionState`, observes `tool.search` `Output::data_json`, promotes only deferred matches into the next prompt snapshot, ignores non-search / failed-search outputs, rejects malformed successful structured data without mutating state, and exposes promotion snapshots/stats/options; `agent::Loop` owns the first fake-provider-backed turn driver (`LoopOptions`, `RunTurnInputs`, `RunTurnResult`) that builds a `prompt::RenderedPrompt`, maps prompt-cache hints through `provider::make_prompt_cache_hints`, mirrors active/promoted tools into name-sorted `provider::Request::tools`, sends requests through `provider::System`, and can now run the sequential direct-dispatch tool path when callers supply non-owning `tool::Registry*` and `tool::DispatchContext*`: it appends assistant tool-use messages, dispatches each `ToolUseContent` through `Registry::dispatch` in original order, appends `Role::tool` / `ToolResultContent` blocks, rebuilds the prompt, re-enters the provider until a terminal text-style response or `LoopOptions::max_iterations`, aggregates provider usage, returns the complete transcript tail, forwards ordinary provider errors unchanged, converts model-repairable tool errors into tool-result error blocks, annotates parent-cancelled provider/tool awaits with `reason=parent_cancelled` plus `cancellation_phase=provider|tools`, writes provider/tool cancelled trace rows before returning cancellation errors when trace is enabled, writes `stop_reason=error` trace rows for non-cancelled provider failures and response-backed loop-boundary failures, generates a non-zero turn id when an enabled trace writer is configured and callers leave `RunTurnInputs::turn_id` unset, threads that id into direct dispatch audit rows only when `TraceContext::enabled` is true, clears/restores reusable context ids for explicit trace-disabled turns, refreshes `DispatchContext::now` around every direct dispatch so registry-owned `permission_ask_rendered` approvals use a real per-call timestamp while restoring the caller's previous context value, and, when enabled `RunTurnInputs::trace` supplies a `storage::TraceRepository`, writes one body-free `trace_turns` row before returning terminal-success responses, and writes a `stop_reason=error` trace row when `LoopOptions::max_iterations` is exhausted by repeated tool_use responses before returning the existing `Error::internal` with `reason=iteration_cap`; (slice 116) `agent::ToolScheduler` opens the spec-0012 batched-dispatch surface as a skeleton: `<oran/agent/scheduler.hpp>` exports `ToolSchedulerOptions { max_parallel_tools=4, per_call_timeout=60 s, idle_lock_ttl=300 s }`, `ToolBatchCall { tool_use_id, name, input_json }`, `ToolBatchResult { tool_use_id, name, output }`, and pimpl-backed `ToolScheduler::run_batch(batch, prototype)`; the skeleton uses an `async::Channel<std::monostate>` filled with one permit per slot for bounded parallelism, races `Registry::dispatch` against `async::sleep_for` via `asio::experimental::awaitable_operators::operator||` for per-call timeout (`Error::cancelled` with `reason=timeout` + `tool` + `per_call_timeout_ms` context), propagates parent cancellation via one `asio::cancellation_signal` per spawned call (held in a `std::deque<asio::cancellation_signal>` for stable addresses because the type is neither copyable nor movable), brace-initialises a fresh `tool::DispatchContext` per call from the caller's prototype so concurrent dispatches do not race on `registry`/`resolved_path`/`approval_token_output`/`now`, and returns ordered results in original `tool_use` order regardless of execution order; (slice 117) adds the per-canonical-path read/write lock table: tools declaring `Capability::write_file`, `edit_file`, or `delete_path` take an exclusive lock and tools declaring `read_file` or `list_directory` take a shared lock, keyed by the workspace-resolved absolute path the scheduler derives from the prototype's `tool::Workspace` plus the JSON `path` field; tools without a lock-class capability or without a resolvable path fall through to bounded-parallelism only; the lock table is private to `src/oran-agent/_impl/path_lock_table.hpp` (single-strand by contract, mirroring `core::BoundedCache`), tracks readers/writer/waiter counts per entry, wakes the next FIFO batch on release (consecutive shared waiters fan out, an exclusive waiter at the front blocks new shared acquirers from skipping the line), handles cancellation during wait without orphaning the queue or leaking a permit, and reaps entries idle past `ToolSchedulerOptions::idle_lock_ttl` on demand; `ToolScheduler` exposes the `ToolSchedulerLockStats` snapshot (`shared_acquires`, `exclusive_acquires`, `contended_acquires`, `cancelled_acquires`, `reaped_entries`, `current_entries`, `peak_entries`) for `--explain-rules`-style consumers and `reap_idle_locks(core::Time)` so the future periodic tick (or tests) can drive the TTL sweep; the scheduler is wired through `agent::Loop` (slice 120): the loop dispatches every tool batch (including N=1) via `scheduler.run_batch` instead of the sequential `for (use : tool_uses) registry.dispatch(...)` loop, and `bench/agent/scheduler_overhead` + `scheduler_audit_fanout` ship; slice 101 drives `agent::Loop` through bootstrap's `AgentPromptRunner` when a caller supplies a backend, and slice 112 drives it from configured-route `bootstrap::run`; (slice 118) approval gating + same-row audit usage enrichment under parallelism are verified, (slice 119) parent cancellation ends every cancel-aware in-flight call within a 100 ms grace window (`kCancellationGrace`) — `run_batch` emits each child's cancel signal, disables its own cancellation filter, races the remaining drain against `async::sleep_for(kCancellationGrace)`, and returns `Error::cancelled` `reason=parent_cancelled` rather than waiting unbounded; a handler that ignores its cancellation slot is stopped-awaiting at the deadline and named in a `cancellation_lag` audit row (`event_kind=cancellation_lag`, `metadata_json.error_kind=cancellation_lag`); (slice 120) `bootstrap::AgentPromptRunner` constructs and owns a persistent `ToolScheduler` from the `runtime.tool_scheduler.{max_parallel_tools, per_call_timeout_ms, idle_lock_ttl_ms}` config block (defaults 4 / 60000 / 300000) and threads it into `RunTurnInputs::scheduler` (a caller that omits it gets a per-turn fallback with default options), completing the tool-scheduler v1 arc; memory and provider SSE streaming remain planned outside the plan | `oran-core`, `oran-async`, `oran-storage`, `oran-prompt`, `oran-tool`, `oran-provider`; planned `oran-memory`, `oran-permission`, `oran-hook` |
| `oran-orchestration` | team + mailbox + coordination strategies | `oran-agent`, `oran-async` |
| `oran-automation`    | cron / periodic / triggered jobs | `oran-agent`, `oran-storage`, `oran-async` |
| `oran-channel`       | `Channel` trait + adapters | `oran-agent`, `oran-async`, `oran-http` |
| `oran-channel-qq`    | QQ adapter (optional, gated) | `oran-channel`, `oran-http` |
| `oran-channel-discord` | Discord adapter (optional, gated) | `oran-channel`, `oran-http` |
| `oran-channel-slack` | Slack adapter (optional, gated) | `oran-channel`, `oran-http` |
| `oran-channel-telegram` | Telegram adapter (optional, gated) | `oran-channel`, `oran-http` |
| `oran-channel-webhook` | generic webhook adapter | `oran-channel`, `oran-http` |
| `oran-web`           | HTTP web UI (cpp-httplib in skeleton, asio later) | `oran-agent`, `oran-orchestration`, `oran-http` |
| `oran-cli`           | REPL / single-shot mode parser with deterministic no-runner shell plus slice-100 `PromptRunner` / `run_async` seam for caller-owned async prompt execution, slice-95 `OperatorPromptSink` for terminal `permission_ask_rendered` approvals (scripted answers for tests/noninteractive drivers, asio stdin read for interactive answers), and (slice 123) `cli::StreamingPromptSink` — a `provider::EventSink` that renders answer/thinking deltas to an injectable `std::ostream` (default `std::cout`, flushed per delta for character-by-character output) plus a one-line `[tool: <name>]` marker per tool call, and reports `rendered_answer_text()` so the prompt runner can suppress the duplicate final-text print; planned slash commands | currently `oran-core`, `oran-async`, `oran-hook`, `oran-provider`; planned `oran-orchestration` |
| `oran-bootstrap`     | process entry + config loading + provider-route preflight + CLI handoff + slice-101 `AgentPromptRunner` (`cli::PromptRunner` implementation that borrows `RuntimeAssembly`, config, resolved route, executor, and a caller-supplied `provider::System`; owns builtin tool registration, permission materialization, `provider::execution::Runtime`, CLI operator-prompt sink binding, `agent::Loop`, trace context, and transcript state) + slice-111 `HttpProviderBackend` (`HttpProviderBackend::build(config, options)` resolves route profiles, builds the adapter plan, reads configured API-key env vars at the explicit credential boundary, owns an `oran-http::Client` on the caller-provided blocking executor, adapts it to `provider::ProtocolTransport`, registers the built-in Anthropic/OpenAI protocol factories, and returns a profile-routed `provider::System` plus the resolved route for runner owners; (slice 123) that `HttpProtocolTransport` now overrides `supports_streaming()` to `true` and implements `send_streaming` over `http::Client::send_streaming`, translating each `http::SseEvent` into the provider SSE callback, and `AgentPromptRunner` constructs a `cli::StreamingPromptSink` for non-quiet streaming runs — passing it to `agent::Loop::run_turn` and clearing the assembled `PromptRunResult::text` once the answer streamed live — so configured-route `orangutan --prompt` over Anthropic renders tokens character-by-character) + `--explain-rules` (with `--mode` / `--agent` selectors) + `--audit-init` + (slice 88) `--trace <turn-id>` operator inspector (32-char lowercase hex) that opens the workspace audit DB, runs the idempotent migration, looks up the trace row through `TraceRepository::get_turn`, lists joined audit rows through `AuditRepository::list_events_for_turn`, and renders both in `--explain-rules`-style lines (returns `Error::not_found` for missing DB or unknown turn id; slice 93 prints `kind=<event_kind>` for each audit row so `hook_publish` rows are distinguishable) + per-process `RuntimeAssembly` (bundles a fresh `permission::ApprovalBroker`, the active `permission::AuditSink`, the slice-41 assembly-owned `tool::Workspace` built from `permissions.workspace.extra_{read,write}_roots`, (slice 87) an optional `storage::TraceRepository` on the shared audit `Pool` when `config.trace().enabled` is `true` so the agent loop inherits spec-0018's per-turn writer, and (slice 92) the process `hook::Bus` configured from `config.hooks.timeout_ms`; defaults to `audit_enabled=true`, `trace_enabled=true`, and a 2000 ms hook timeout now that the migrations and hook foundation ship inside the binary) + the slice-23 `SignalScope` SIGINT/SIGTERM trap (`asio::signal_set` RAII, `release()` cancel + `signum()` capture) that `--audit-init` and `--trace` adopt so Ctrl-C / `kill` interrupt the one-shot `io_context` drain promptly, with `bootstrap::run` translating the resulting `Error::cancelled` into the shell-conventional `128 + signum` exit code | currently `oran-core`, `oran-async`, `oran-http`, `oran-io`, `oran-storage`, `oran-config`, `oran-permission`, `oran-hook`, `oran-tool`, `oran-provider`, `oran-agent`, `oran-cli`; planned every public lib above |

**Binaries** built on top:

| Binary             | Description                                                  |
| ------------------ | ------------------------------------------------------------ |
| `orangutan`        | Default: CLI REPL or single-shot, optional `--web` and channel modes. |
| `orangutan-server` | Daemon mode: web + channels + automation, no terminal UI.    |
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
1.  Inbound message arrives via cli / web / channel / automation.
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
  to `oran-cli` through `cli::run_async` with `AgentPromptRunner`. Built-in empty
  defaults still use the deterministic no-runner shell, and tests also exercise the
  runner seam with `provider::FakeProvider`.
- The current typed surface covers `strict_config`, `runtime` (including
  `tool_output` caps and `prompt.active_tools`), `profiles` (including
  optional `protocol`), `routes`,
  `session`, `web`, `trace`, `hooks.timeout_ms`, `permissions`, and
  `agents.<name>.permissions`; planned sections such as channels, teams,
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
