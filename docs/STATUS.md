# Current State

> **One-screen project snapshot.** Read this *first* in every new session to
> orient on where the project is right now. Update in the same commit as
> the history entry that moves it — see
> [`rules/docs-in-sync.md`](rules/docs-in-sync.md).

## Snapshot

- **Slice:** 108 (`xmake run orangutan` reports slice 108)
- **Last completed history:**
  [`histories/2026-05/20260526-0228-provider-protocol-response.md`](histories/2026-05/20260526-0228-provider-protocol-response.md)
- **Active exec-plan:**
  [`exec-plans/active/2026-05-26-provider-adapter-v1.md`](exec-plans/active/2026-05-26-provider-adapter-v1.md)
  — tracks the remaining multi-slice provider adapter handoff from offline
  protocol bytes through transport-backed factories and ordinary binary
  `cli::run_async` wiring.
- **Next intended slice:** Continue along the spec dependency graph
  (0013 → 0011 + 0012 → 0014 → 0016 → 0017 → 0015 → 0018). Slice 108 adds
  the provider-owned offline protocol response decoding boundary needed before
  HTTP transport-backed factories. New `<oran/provider/protocol_response.hpp>`
  exports `provider::decode_protocol_response(body_json, target)`. The decoder
  supports `ProtocolKind::anthropic_messages` and
  `ProtocolKind::openai_responses`, keeps `nlohmann_json` private to the
  provider `.cpp`, maps vendor text, thinking/reasoning summaries, tool-use
  blocks, model ids, token usage, and status/stop reasons into the typed
  `provider::Response` contract, maps unknown vendor stop/status values to
  `StopReason::error`, and rejects malformed JSON or unsupported item shapes
  as `ErrorKind::parsing` with non-secret context. `bootstrap::run` still does
  not read provider credentials, construct adapters, allocate transport, send a
  network request, or start `agent::Loop` for ordinary binary prompts. Focused
  result: `test-provider` 57 cases / 442 assertions. Remaining handoff work is
  now transport-backed Anthropic/OpenAI protocol factories and switching
  `bootstrap::run` to `cli::run_async` only when that backend exists. Slice 107 adds
  the provider-owned offline protocol request serialization boundary needed
  before HTTP transport. New `<oran/provider/protocol_request.hpp>` exports
  `provider::ProtocolRequest { method, path, body_json }` and
  `provider::make_protocol_request(request, target)`. The mapper supports
  `ProtocolKind::anthropic_messages` and `ProtocolKind::openai_responses`,
  converts the typed `provider::Request` / `core::Message` / `core::Content`
  contract into vendor JSON body bytes, validates opaque tool schema,
  tool-input, and structured tool-result JSON in the provider `.cpp`, maps
  text-only tool results to the existing text fallback, maps
  `ToolResultContent::data_json` into Anthropic `tool_result.content[]` or
  serialized OpenAI Responses `function_call_output.output`, and rejects
  unsupported protocol families as `Error::config`. `core::ToolResultContent`
  now preserves optional `data_json`, and `agent::Loop` copies successful
  `tool::Output::data_json` into the provider-facing tool-result transcript
  so spec-0014 structured bytes reach the protocol mapper. `oran-provider`
  now uses `nlohmann_json` privately for serialization; public headers still
  expose only bytes and stdlib value types. `bootstrap::run` still does not
  read provider credentials, construct adapters, allocate transport, send a
  network request, or start `agent::Loop` for ordinary binary prompts.
  Focused results: `test-core` 71 cases / 455 assertions, `test-provider`
  51 cases / 398 assertions, and `test-agent` 25 cases / 401 assertions.
  Remaining handoff work after that slice was response decoding, transport-backed
  Anthropic/OpenAI protocol factories, and switching `bootstrap::run` to
  `cli::run_async` only when that backend exists. Slice 106 adds
  the provider-owned adapter factory dispatch seam that consumes slice 105's
  credential bundle without introducing HTTP transport. New
  `<oran/provider/adapter_factory.hpp>` exports
  `provider::ProtocolAdapterFactory`,
  `provider::ProtocolAdapterFactoryBinding`, and
  `provider::make_adapter_system(credentials, factories)`. The factory builds
  one concrete backend per primary/fallback credential target by matching each
  target's `adapter_name` to a caller-registered protocol factory, rejects
  missing/null/duplicate bindings and duplicate route profiles as
  `Error::config`, and returns a profile-routed `provider::System`. That
  returned system expects the execution layer to pass a single selected target
  per call, forwards a one-target route to the matching backend, and leaves
  retry/fallback ownership in `provider::execution::Runtime`. `bootstrap::run`
  does not call this boundary yet, so ordinary startup still preflights
  route/profile/adapter metadata without reading provider credentials,
  decrypting secrets, allocating an HTTP client, constructing a real adapter,
  sending a network request, or starting `agent::Loop` for ordinary binary
  prompts. Focused result: `test-provider` 45 cases / 329 assertions.
  Remaining handoff work is still implementing concrete Anthropic/OpenAI
  protocol factories, wiring transport, and switching `bootstrap::run` to
  `cli::run_async` only when that backend exists. Slice 105 adds
  the explicit provider credential-resolution boundary that a future concrete
  adapter factory will call after slice 104's offline plan. New
  `<oran/provider/credentials.hpp>` exports
  `provider::AdapterCredentialTarget`, `provider::AdapterCredentialBundle`,
  and `provider::resolve_adapter_credentials(plan)`. The resolver reads the
  environment variables named by each plan target's `api_key_env`, stores only
  in-memory API-key strings beside the existing adapter-plan target, derives
  the same loop-facing `provider::Route`, returns `ErrorKind::auth` for
  missing or empty API-key env vars, and keeps error context to non-secret
  fields (`role`, `profile`, `api_key_env`). `bootstrap::run` does not call
  this boundary yet, so ordinary startup still preflights route/profile/adapter
  metadata without reading provider credentials, decrypting secrets, allocating
  an HTTP client, constructing an adapter, sending a network request, or
  starting `agent::Loop` for ordinary binary prompts. Focused result:
  `test-provider` 36 cases / 259 assertions. Remaining handoff work is still
  constructing real Anthropic/OpenAI provider systems from config and switching
  `bootstrap::run` to `cli::run_async` only when that backend exists. Slice 104 adds
  the offline provider adapter construction plan that consumes slice 103's
  route-profile bundle. `<oran/provider/adapter_plan.hpp>` now exports
  `provider::AdapterConstructionTarget`, `provider::AdapterConstructionPlan`,
  and `provider::make_adapter_construction_plan(resolution)`. The plan keeps
  each resolved `ResolvedProfileTarget` beside the protocol adapter name that
  a future concrete factory will dispatch on, derives the existing loop-facing
  `provider::Route`, and preflights non-empty provider/model/base-url/API-key
  env metadata plus `http://` / `https://` endpoint schemes. `bootstrap::run`
  now resolves the configured `default` route profiles and builds this offline
  plan before CLI handoff, preserving the same non-secret startup summary; it
  still does not read environment variables, decrypt credentials, allocate an
  HTTP client, construct an adapter, send a network request, or start
  `agent::Loop` for ordinary binary prompts. Focused result:
  `test-provider` 32 cases / 233 assertions and `test-bootstrap` 65 cases /
  269 assertions. Remaining handoff work is still constructing real
  Anthropic/OpenAI provider systems from config and switching `bootstrap::run`
  to `cli::run_async` only when that backend exists. Slice 103 adds
  the route-profile resolution bundle that real provider adapter construction
  needs after the slice-102 protocol seam. `<oran/provider/route_resolver.hpp>`
  now exports `provider::ResolvedProfileTarget`,
  `provider::RouteProfileResolution`, and
  `provider::resolve_route_profiles(config, route_name)`. That richer resolver
  performs the same route/profile/protocol validation as `resolve_route`, but
  keeps the profile endpoint metadata (`provider`, `base_url`, `api_key_env`)
  beside the loop-facing `ModelTarget`; `RouteProfileResolution::route()`
  derives the existing `provider::Route` so `agent::Loop` and
  `provider::execution::Runtime` do not change. `bootstrap::run` now preflights
  that adapter-factory-ready bundle for the configured `default` route while
  preserving the existing non-secret startup summary; it still does not read
  environment variables, decrypt credentials, construct an adapter, send a
  network request, or start `agent::Loop` for ordinary binary prompts. Focused
  result: `test-provider` 28 cases / 210 assertions. Remaining handoff work is
  still constructing real Anthropic/OpenAI provider systems from config and
  switching `bootstrap::run` to `cli::run_async` only when that backend exists.
  Slice 102 closes
  the provider-profile protocol seam that was still implicit after the slice-98
  route resolver. `config::ProfileConfig` now carries optional
  `protocol`; `oran-config` parses `profiles.<name>.protocol` as a
  non-empty string, `config.example.json` documents
  `"protocol": "anthropic_messages"` on the default profile, and
  `provider::resolve_route` prefers that explicit exact `ProtocolKind`
  spelling before falling back to provider-label aliases such as `anthropic`,
  `openai`, or `deepseek`. That lets a custom/self-hosted vendor label resolve
  to a shipped wire format without overloading `provider`, while preserving the
  older alias path for existing profiles. Invalid explicit protocols now fail
  with `Error::config` carrying `route` / `profile` / `role` / `protocol`
  context during the same bootstrap preflight path that already caught unknown
  provider labels. Focused result: `test-config` 33 cases / 241 assertions,
  `test-provider` 26 cases / 181 assertions, and `test-bootstrap` 64 cases /
  264 assertions. No provider credentials are read, no adapter is constructed,
  no network request is sent, and ordinary binary prompts still do not start
  `agent::Loop`; remaining handoff work is still constructing real
  Anthropic/OpenAI provider systems from config and switching
  `bootstrap::run` to `cli::run_async` only when that backend exists. Slice 101 adds
  the adapter-neutral bootstrap runner that consumes the slice-100 CLI seam.
  `<oran/bootstrap/prompt_runner.hpp>` now exports
  `AgentPromptRunnerOptions` and `AgentPromptRunner`, a caller-supplied
  `cli::PromptRunner` implementation that borrows a `RuntimeAssembly`,
  config, provider backend, executor, and resolved `provider::Route`.
  `AgentPromptRunner::create` registers the shipped builtin tool catalog,
  materializes permissions from config plus an optional agent overlay, wraps
  the backend in `provider::execution::Runtime`, binds
  `cli::OperatorPromptSink` to the assembly-owned `permission_ask_rendered`
  bus with scripted-answer support for tests, threads workspace/audit/broker/
  hook/output-cap services into `tool::DispatchContext`, carries the
  assembly-owned `TraceRepository` into `RunTurnInputs::trace`, and drives
  `agent::Loop` for each parsed prompt while retaining the transcript across
  calls. Focused result: `test-bootstrap` 63 cases / 259 assertions, including
  CLI-to-loop handoff with trace row persistence, provider retry through the
  execution wrapper, and a broker-backed `file.read` approval through the CLI
  sink. Regular `bootstrap::run` still calls the deterministic no-runner
  `cli::run` path until a real provider adapter factory exists, so no provider
  credentials are read, no network request is sent, and the shipped binary
  still does not start `agent::Loop` on ordinary `--prompt` runs. Remaining
  handoff work: construct real Anthropic/OpenAI provider systems from config
  and switch `bootstrap::run` to `cli::run_async` only when that backend exists.
  Slice 100 opens
  the adapter-neutral CLI prompt-runner handoff seam. `<oran/cli/cli.hpp>`
  now exports `PromptRunRequest`, `PromptRunResult`, `PromptRunner`, and
  `cli::run_async(CliOptions, PromptRunner*)`; `run_async` reuses the existing
  mode parser, delegates single-shot prompts and non-empty scripted REPL lines
  to the caller-owned runner in order, prints non-empty runner text when not
  quiet, and propagates runner errors unchanged. `cli::run` remains the
  deterministic no-runner shell, and `bootstrap::run` still calls that path, so
  no provider credentials are read, no adapter is constructed, no network
  request is sent, and ordinary binary prompts still do not start
  `agent::Loop`. Focused result: `test-cli` 14 cases / 97 assertions. Slice
  101 consumes that seam with the bootstrap-owned runner described above.
  Slice 99 consumes
  the route resolver at the binary boundary. Regular `bootstrap::run` startup
  now preflights the configured `default` provider route whenever config
  declares routes, prints
  `provider route: default primary=<profile>/<model> protocol=<kind>
  fallbacks=<n>` plus ordered fallback rows, and returns the resolver's
  `Error::config` before CLI handoff when route/profile/protocol references are
  invalid. Built-in empty defaults still report `provider route: none
  configured` and continue to the deterministic pre-loop CLI shell; no provider
  credentials are read, no adapter is constructed, no network request is sent,
  and ordinary binary prompts still do not start `agent::Loop`.
  `oran-bootstrap` now declares its direct `oran-provider` dependency. Focused
  result: `test-bootstrap` 59 cases / 230 assertions. Slice 101 now supplies
  the runner that wraps caller-provided provider systems in
  `provider::execution::Runtime`; the remaining real-CLI work is provider
  adapter construction and switching `bootstrap::run` to the async handoff.
  Slice 98 lands
  the config-to-provider route resolver required before loop/binary handoff.
  `<oran/provider/route_resolver.hpp>` exports
  `provider::resolve_route(const config::Config&, std::string_view)`, which
  resolves a named `config::RouteConfig` into the existing `provider::Route`
  value by looking up the primary/fallback `config::ProfileConfig` entries,
  preserving authored fallback order, mapping provider spellings and exact
  `ProtocolKind` names into `ProtocolKind`, and returning `Error::config`
  with `route` / `profile` / `role` context for missing references or unknown
  provider spellings. Slice 102 adds optional `profiles.<name>.protocol` so the
  resolver can use an explicit wire-format spelling before provider-alias
  inference and can report unknown explicit protocols with `protocol` context.
  The current typed config still carries only provider/model/base URL/API-key
  metadata plus that optional protocol field, so resolved targets fill
  `{profile, model, protocol}` and leave `thinking_budget` / `cache` unset
  until those route/profile policy fields land. `oran-provider` now declares
  its direct `oran-config` dependency instead of leaning on the transitive
  `oran-prompt` path, and `<oran/provider.hpp>` re-exports the resolver.
  Focused result through slice 102: `test-provider` 26 cases / 181 assertions.
  Remaining
  provider work: provider request/response hooks, usage/cost rollups, real
  Anthropic/OpenAI adapters, and binary construction of a concrete provider
  backend for the bootstrap runner. Slice 103 adds
  `provider::resolve_route_profiles` as the adapter-factory-ready companion to
  `resolve_route`: it preserves `provider`, `base_url`, and `api_key_env` for
  the primary/fallback profiles while deriving the same loop-facing `Route`
  value. Focused result through slice 103: `test-provider` 28 cases / 210
  assertions. Slice 97 lands the first provider execution layer required before
  real adapter and binary handoff work. `<oran/provider/execution.hpp>` now
  exports
  `provider::execution::Runtime`, a `provider::System` decorator over any
  backend `System`. It consumes `Request::retry.max_attempts` and
  `initial_backoff`, retries retryable `network` / `rate_limit` / `timeout` /
  `upstream` errors on the same target, stops immediately for non-retryable
  errors and cancellations, and after a retryable primary exhaustion tries
  `Route::fallbacks` in order with the same per-target attempt budget. Each
  concrete backend call receives a single-target `Route`, so adapters do not
  implement fallback themselves. Successful responses that omit `model_used`
  are filled with the selected target model, preserving later trace rows when a
  fallback wins. Backoff uses `async::sleep_for` and observes parent
  cancellation. If an attempt has already emitted visible `EventSink` output,
  later retryable errors return immediately with `retry_skipped` /
  `fallback_skipped=stream_already_emitted` so terminal/UI callers do not see
  duplicate stream bytes. `test-provider` adds offline execution coverage for
  same-target retry, fallback success, provider-supplied `model_used`,
  non-retryable stop, stream-output retry suppression, zero-attempt validation,
  and cancellation during retry backoff. Slice 96 closed the
  agent-loop approval-observability gap that sat between the direct
  dispatch ask bridge and the later binary handoff. `agent::Loop` now wraps
  each direct `tool::Registry::dispatch` with a scoped dispatch context that
  installs the loop's trace parent id and refreshes `DispatchContext::now` from
  `core::time::now_utc()`, then restores the caller's reusable `parent_turn_id`
  and `now` values after the call. That makes `PermissionAskRenderedPayload`
  `requested_at`, approval-token expiry, and immediate broker verification use
  the real per-tool-call clock instead of a stale caller value such as the
  default epoch. `test-agent` adds an offline fake-provider turn that asks for
  `file.read`, flows through `permission::ApprovalBroker` plus a blocking
  `hook::InProcessSink` on `permission_ask_rendered`, asserts the prompt
  payload's identity/replay/TTL/request time, records
  `metadata_json.permission_ask_decisions[]`, returns the approved tool result
  to the second provider request, and verifies the issued token against that
  request time. Focused result: `test-agent` 24 cases / 391 assertions. The
  remaining approval work is still binary handoff: bind the existing
  `cli::OperatorPromptSink` into the real CLI agent-loop runtime once
  `orangutan` constructs `agent::Loop` with provider/assembly services. Slice
  95 closed spec-0015's first concrete user-visible approval renderer.
  `oran-cli`
  now exports `cli::OperatorPromptSink` from
  `<oran/cli/operator_prompt_sink.hpp>` and the umbrella `<oran/cli.hpp>`.
  The sink implements `hook::Sink`, handles blocking
  `Event::permission_ask_rendered` payloads, renders the tool name, caller
  identity, matched decision reason, replay/TTL policy, request timestamp,
  and input JSON, then accepts yes/approve/proceed or no/deny/reject style
  answers. Approval returns `HookDecisionKind::proceed` with
  `operator_approved:<identity>` in the sink trace; denial returns
  `HookDecisionKind::veto` with `operator_denied:<identity>`. Test and
  noninteractive callers can provide `scripted_answers`; otherwise the sink
  reads one terminal line through an asio `posix::stream_descriptor` on the
  current coroutine executor. `oran-cli` now depends on `oran-async` and
  `oran-hook`. Focused result: `test-cli` 10 cases / 68 assertions. The
  remaining approval work is binary handoff: bind this sink into the real
  CLI agent-loop runtime once `orangutan` drives `agent::Loop` with a real
  provider adapter. Slice 94 closed the direct-dispatch half of
  spec-0015's `permission_ask_rendered` round-trip.
  `<oran/hook/payload.hpp>` now carries
  `hook::PermissionAskRenderedPayload { tool_name, input_json, who,
  decision_reason, replay_max, approval_ttl, requested_at }` in the public
  `hook::Payload` variant. `tool::DispatchContext` gains an optional
  `approval_token_output` slot. When a permission rule returns `ask`, a
  broker is present, no caller-supplied token exists, and a bus is attached,
  `Registry::dispatch` publishes blocking
  `Event::permission_ask_rendered`. A subscribed sink returning `proceed`
  issues a broker grant using the matched rule's TTL/replay policy, stores
  the token for the caller when requested, immediately verifies it, records
  `outcome=approved`, and runs the handler. A sink returning `veto` records
  `outcome=rejected`, `reason=operator_denied`, skips the handler, and returns
  `Error::permission_denied` with `reason=operator_denied` plus the sink
  reason as `hook_reason`. Unsupported ask decisions (`rewrite` /
  `require_approval`) are rejected the same way, and buses with no subscribed
  ask sink preserve the legacy `approval_required` short-circuit. Permission
  ask sink traces are serialized under
  `metadata_json.permission_ask_decisions[]`. Focused result: `test-tool` 178
  cases / 1838 assertions.
  Slice 93 closes
  spec-0018 AC5 for direct `tool_before` blocking publishes. The audit DB
  migration stream now reaches version 4:
  `src/oran-storage/migrations/audit/0004-audit-event-kind.sql` adds
  `audit_events.event_kind TEXT NOT NULL DEFAULT 'permission_decision'` plus a
  parent-turn index for hook rows. `storage::AppendAuditEventRequest`,
  `UpdateAuditEventMetadataRequest`, `AuditEventRecord`, and
  `ListAuditEventsOptions` expose the discriminator, and update-metadata
  matching includes it so ordinary permission-row enrichment cannot clobber a
  same-tool `hook_publish` row. `permission::AuditEvent` and
  `AuditMetadataUpdate` carry the same field through `RecordingAuditSink` and
  `StorageAuditSink`. `tool::Registry::dispatch` now writes an extra
  `event_kind=hook_publish` audit row after a blocking `tool_before` publish
  when `DispatchContext::parent_turn_id` is set and the bus returned consulted
  sink traces; the row uses the same parent turn id and serializes
  `metadata_json.event`, `sink_id`, `decision_kind`, `reason`, optional
  `elapsed_ms` / `error`, and the full `hook_decisions[]` trace before the
  existing permission-decision row is recorded. `orangutan --trace` already
  joins the new row through `AuditRepository::list_events_for_turn`; its audit
  line now prints `kind=<event_kind>` so operators can distinguish hook
  publishes from permission decisions. Focused results: `test-storage` 72 cases
  / 899 assertions, `test-permission` 89 / 426, `test-tool` 174 / 1769, and
  `test-bootstrap` 57 / 226. Slice 95 closes the v1 operator-prompt sink;
  the remaining spec-0015/0018 item here is the binary handoff that drives
  `agent::Loop` from inside the `orangutan` binary once a real provider
  adapter exists. Slice 92 closes
  spec-0015's direct-dispatch blocking timeout follow-up. `oran-config`
  now parses the top-level `hooks.timeout_ms` policy as a positive integer
  with a default of 2000 ms, `config.example.json` documents that default,
  and `bootstrap::run` threads the parsed value into
  `RuntimeAssemblyOptions::hook_blocking_timeout`. `RuntimeAssembly` now owns
  the process `hook::Bus` alongside the broker/audit/workspace/trace bundle
  and constructs it with `hook::BusOptions{blocking_timeout}`; the startup
  banner reports `hook-timeout=<ms>`. `hook::Bus::publish_blocking<E>` races
  each consulted blocking sink against `async::sleep_for` on the coroutine
  executor. A timed-out sink synthesizes a `veto` with
  `reason=hook_timeout`, records `HookDecisionTrace::elapsed`, short-circuits
  later blocking sinks, and lets direct `Registry::dispatch` reuse the slice-91
  `blocked_by_hook` path: handler skipped, advisory failure events published,
  audit row `outcome=blocked_by_hook`, and
  `metadata_json.hook_decisions[].elapsed_ms` persisted. Focused results:
  `test-config` 32 cases / 235 assertions, `test-hook` 30 / 207,
  `test-bootstrap` 57 / 224, and `test-tool` 173 / 1739. Slice 91 consumes
  spec-0015's blocking `tool_before` surface inside direct
  `tool::Registry::dispatch`: `<oran/hook/decision.hpp>` now carries
  `HookDecisionTrace { sink_id, kind, reason }` and
  `HookDecision::trace`, and `hook::Bus::publish_blocking<E>` fills that
  vector in subscription order for every sink it actually consults. Dispatch
  now calls `publish_blocking<Event::tool_before>` before workspace
  pre-resolution and permission evaluation; `veto`, hook-error, and malformed
  `rewrite` decisions record `permission::AuditOutcome::blocked_by_hook`,
  skip the handler, publish advisory `tool_error` / `tool_after` with
  `error_kind=blocked_by_hook`, and return `Error::permission_denied`.
  Valid `rewrite` decisions substitute the effective input before workspace
  resolution, permission evaluation, broker checks, audit, handler execution,
  and later hook payloads; allowed rewritten calls record
  `AuditOutcome::rewritten`, row `input_hash=SHA-256(rewritten_input)`, plus
  `metadata_json.original_input_hash`, `rewritten_input_hash`, and
  `hook_decisions`. `require_approval` promotes otherwise-allow decisions into
  the existing `ApprovalBroker` path while preserving an underlying permission
  deny. `permission::AuditOutcome` now exports the two new enum values, and
  `bench-hook` adds blocking-publish scenarios for no sinks, one sink,
  three all-proceed sinks, and a second-sink short-circuit. Focused results:
  `test-hook` 29 cases / 196 assertions, `test-permission` 89 / 426, and
  `test-tool` 172 / 1722. Slice 89 closes
  spec-0018 AC12 by adding `bench/storage/scenarios/trace_turn_insert.cpp`,
  a single-insert A-vs-B pair for `trace_turns`:
  `storage.trace_turn_insert_raw_pool` runs one raw `Pool` +
  `StatementCache` INSERT per nanobench iteration and
  `storage.trace_turn_insert_repository` runs one
  `TraceRepository::append_turn` per iteration. Both use unique per-iteration
  turn ids and their own temp DB so the bench measures steady-state
  per-turn cost without batch overhead. Initial WSL2 numbers report about
  13 µs / insert for the raw path and about 16 µs / insert for the
  repository wrapper — both comfortably inside the spec's ≤ 50 µs
  target. Adjacent to the new scenario, the existing
  `scenarios/trace_repository.cpp` (32-row batch) needed an `id_for`
  collision fix: the overlapping-sum encoding `salt + row + i + (batch &
  0x0f)` produced identical bytes for distinct `(batch, row)` tuples — for
  example `(0, 1)` and `(1, 0)` — and the trace `PRIMARY KEY` guard
  aborted the second nanobench epoch. The fix packs `(salt, row, batch)`
  into non-overlapping byte ranges so every tuple maps to a distinct id,
  and the batch scenarios now report stable numbers alongside the new
  single-insert pair. `test-storage` still reports 72 cases / 886
  assertions; the change is bench-only.
  Slice 88 closes spec-0018 AC10 by adding the operator inspector: `oran-storage` exports
  `AuditRepository::list_events_for_turn(TurnId, limit)` — a `parent_turn_id =
  ?` read ordered `id ASC` so the original `tool_use` order of a spec-0017
  multi-tool turn survives the trace/audit join — and `oran-bootstrap`'s
  `--trace <turn-id>` / `--trace=<turn-id>` flag opens the workspace audit
  DB, runs the idempotent audit migration, looks up the trace row through
  `TraceRepository::get_turn`, lists the joined audit rows through the new
  repository method, and prints both in the `--explain-rules`-style line
  format before exiting zero. The inspector returns `Error::not_found` for
  a missing audit DB and for an unknown turn id, propagates SIGINT/SIGTERM
  through the existing `SignalScope` so the one-shot `io_context` drains
  promptly, and accepts the 32-char lowercase hex spelling that storage
  round-trips through `BLOB`. `test-storage` now reports 72 cases / 886
  assertions and `test-bootstrap` reports 56 cases / 221 assertions. Hook
  publish rows, the bench `trace_turn_insert` scenario, and the binary
  handoff that drives `agent::Loop` from inside the binary remain
  downstream. Slice 87 closed
  the first downstream item on the spec-0018 punch list by threading
  `config.trace().enabled` from `oran-config` through `bootstrap::run` into
  the new `RuntimeAssemblyOptions::trace_enabled` switch and constructing
  a `storage::TraceRepository` on the assembly-owned audit `Pool` when
  both audit and trace are enabled. `RuntimeAssembly` now exposes
  `trace_repository()` (non-null only when `trace_enabled()` returns true)
  so the upcoming agent-loop owner can plug the repository into
  `agent::TraceContext` without minting a second DB handle. The bootstrap
  startup banner reports `trace=enabled|disabled` alongside the existing
  audit/workspace summary, and `test-bootstrap` now reports 51 cases /
  188 assertions covering the default-on path (smoke `append_turn`),
  explicit trace-off, and audit-disabled-forces-trace-off cases. Hook
  publish rows, CLI `--trace`, and binary handoff remain downstream.
  Slice 86 closes
  the last loop-owned spec-0018 writer gap by persisting iteration-cap exits.
  When `LoopOptions::max_iterations` is exhausted by repeated tool_use
  responses and `RunTurnInputs::trace` has an enabled `TraceRepository`,
  `agent::Loop` now writes a body-free `trace_turns` row with
  `stop_reason=error`, `iteration_count = LoopOptions::max_iterations`, the
  final iteration's rendered prompt prefix hash/bytes and active/deferred
  catalog hashes, the aggregated provider usage, and the last response's
  model id (falling back to the primary route model when the final response
  omitted one). The existing `Error::internal` with `reason=iteration_cap`
  is still returned unchanged afterwards, and trace-disabled or
  repository-less callers still take the legacy no-row path.
  `test-agent` now reports 23 cases / 363 assertions. Config-to-loop wiring,
  hook publish rows, CLI `--trace`, and binary handoff remain downstream.
  Slice 85 lands
  loop-owned trace turn-id generation for spec-0018. When
  `RunTurnInputs::trace` has an enabled `TraceRepository` and the caller does
  not provide `RunTurnInputs::turn_id`, `agent::Loop` now generates a non-zero
  version/variant-shaped 16-byte turn id before the first prompt render. The
  generated id is used for the `trace_turns.turn_id` row and for every direct
  `audit_events.parent_turn_id` stamped during that turn, preserving the
  trace/audit join without requiring test-only or future bootstrap callers to
  pre-fill the id. Trace-disabled and repository-less pre-trace callers still
  keep `parent_turn_id = NULL` unless they explicitly supply a turn id.
  `test-agent` now reports 22 cases / 345 assertions. Config-to-loop wiring,
  hook publish rows, CLI `--trace`, and binary
  handoff remain downstream. Slice 84 lands the
  first ordinary error trace rows for spec-0018's loop-owned writer.
  `agent::Loop` now writes a durable `trace_turns` row with
  `stop_reason=error` before returning a non-cancelled provider error, using
  the primary route model because no provider response exists. It also writes
  `stop_reason=error` rows for response-backed loop-boundary failures:
  `tool_use` responses without caller-supplied dispatch services,
  `tool_use` stop reasons without tool blocks, unsupported non-terminal stop
  reasons, and non-cancelled storage/internal direct-dispatch failures. Those
  rows use the provider response model when present and preserve the aggregate
  usage observed before the error. Parent-cancelled provider/tool paths still
  take the slice-83 `cancelled` writer and do not perform any extra await while
  a terminal cancellation is active unless the cancellation row path has first
  reset the coroutine cancellation state. Config-to-loop wiring, hook publish
  rows, CLI `--trace`, and binary handoff remain downstream. Slice 83 lands the
  first cancellation trace rows for spec-0018
  AC4. `agent::Loop` now writes a durable `trace_turns` row with
  `stop_reason=cancelled` and `cancellation_phase=provider|tools` when parent
  cancellation lands during the provider await or direct tool dispatch and
  `RunTurnInputs::trace` is enabled. The writer briefly resets the coroutine
  cancellation state only for the trace insert so the audit row can survive the
  cancellation that caused it; the returned error remains
  `ErrorKind::cancelled` with `reason=parent_cancelled`. Provider-phase rows
  use the primary route model because no provider response exists; tool-phase
  rows use the provider response model and aggregate usage observed before the
  tool cancellation. Slice 82 lands the
  explicit trace-disabled loop policy required by spec-0018 AC9.
  `agent::TraceContext` now has an `enabled` switch that defaults to true for
  existing trace-enabled and pre-trace callers. When callers set
  `RunTurnInputs::trace.enabled=false`, `agent::Loop` writes zero
  `trace_turns` rows even if a `TraceRepository` is present, threads
  `std::nullopt` into direct tool dispatch so new audit rows keep
  `audit_events.parent_turn_id = NULL`, and restores any reusable
  `tool::DispatchContext::parent_turn_id` after the dispatch finishes.
  `test-agent` covers the policy with a storage-backed single-tool turn.
  Bootstrap still does not map
  `config::TraceConfig` into loop inputs; hook publish rows, CLI `--trace`,
  and binary handoff remain downstream. Slice 81 lands the
  typed operator trace policy surface: `config::TraceConfig` and
  `Config::trace()` parse the top-level `trace.enabled`,
  `trace.store_raw_bodies`, and `trace.retention_days` block documented by
  spec 0018, with defaults `{true, false, 30}` and config-time validation for
  boolean flags plus positive integer retention. `config.example.json` carries
  the default block, and `test-config` covers custom values, the example file,
  and malformed trace policy (30 cases / 225 assertions). The parsed config is
  still not wired through bootstrap; slice 82 adds the equivalent explicit loop
  switch on `RunTurnInputs::trace`, and trace rows still require the
  caller-supplied trace context from slice 80. Slice 80 lands the
  first loop-owned spec-0018 `trace_turns` writer for terminal-success fake
  provider turns. `agent::RunTurnInputs::trace` carries a non-owning
  `storage::TraceRepository*`, `session_id`, optional `parent_turn_id`,
  `agent_key`, `origin`, and redacted `context_json`; when callers also supply
  `RunTurnInputs::turn_id`, or slice 85 generates one for a configured trace
  writer, `agent::Loop` awaits one `TraceRepository::append_turn` before
  returning terminal `end_turn` / `stop_sequence` / `max_tokens` results.
  The row records route profile/model, start/finish timestamps, stop reason,
  iteration count, prompt prefix hash/bytes, active/deferred catalog hashes,
  aggregate provider usage tokens/cost, cache token counters, and body-free
  context bytes. The existing direct-dispatch audit path still stamps
  `audit_events.parent_turn_id` with the same turn id, so a single-tool loop turn
  now has both sides of the cause-chain join. `test-agent` covers single-text
  trace rows, storage-backed tool-audit correlation, the slice-82 disabled
  policy case, the slice-83 provider/tool cancellation trace rows, and the
  slice-84 provider/loop-boundary error trace rows, and the slice-85 generated
  turn-id trace/audit join path (22 cases / 345 assertions). Iteration-cap
  trace rows, config-to-loop wiring, hook publish rows, CLI `--trace`, and
  binary handoff remain downstream.
  Slice 79 threads the first spec-0018 cause-chain id through the direct
  tool-dispatch path. `oran-core` now owns `core::TurnId`, the shared 16-byte
  value shape used by storage trace ids and audit correlation. `storage::TraceId`
  aliases it; audit DB migration
  `src/oran-storage/migrations/audit/0003-audit-parent-turn-id.sql` adds the
  nullable `audit_events.parent_turn_id` BLOB plus an index, so the embedded
  audit/trace migration stream now reaches version 3. `AppendAuditEventRequest`,
  `UpdateAuditEventMetadataRequest`, `AuditEventRecord`,
  `permission::AuditEvent`, and `AuditMetadataUpdate` all expose optional typed
  `parent_turn_id`; `StorageAuditSink` persists it; and same-row metadata
  enrichment matches it so concurrent same-tool calls from different turns do
  not clobber each other. `tool::DispatchContext` carries the optional parent
  turn id into `Registry::dispatch`, and `agent::RunTurnInputs::turn_id` is the
  loop-owned source for direct tool calls: traced turns stamp every dispatch
  with that id, while explicit trace-disabled turns force `parent_turn_id = NULL`
  during dispatch and restore any reusable context value afterward. `test-core` covers
  the value type (70 cases / 453 assertions), `test-storage` covers audit
  version-3 migration, BLOB round-trip, metadata update scoping, and zero-id
  validation (70 cases / 856 assertions), `test-permission` covers
  recording/storage sink propagation (89 cases / 414 assertions), and
  `test-tool` covers registry audit stamping (166 cases / 1590 assertions).
  Slice 78 opened the
  storage foundation for spec 0018: `oran-storage` exports
  `TraceRepository`, `TraceId` (16-byte BLOB at the database boundary),
  `AppendTraceTurnRequest`, `TraceTurnRecord`, `ListTraceTurnsOptions`, and
  `built_in_trace_migrations()`. Slice 77 extends
  the real `agent::Loop` driver from the slice-76 sequential tool loop into
  the first cancellation-phase classification needed by specs 0017/0018.
  Provider-await cancellations and tool-dispatch cancellations still return
  `ErrorKind::cancelled`, but the loop now adds
  `reason=parent_cancelled` plus `cancellation_phase=provider|tools` before
  returning the error; slice 83 also writes matching cancelled trace rows when a
  trace context is configured. Slice 78 introduced the trace schema and
  repository, and slice 80 wires terminal-success rows.
  Ordinary provider errors, retryable network/upstream failures, storage
  failures, and model-repairable tool errors keep their existing return
  behavior; trace-enabled turns now record provider and response-backed
  loop-boundary failures as `stop_reason=error`.
  `test-agent` covers both cancellation phases through parent
  `asio::cancellation_signal` tests.
  Iteration-cap trace rows, approval-observability coverage, and the
  trace/audit inspector rows are now in place; slice 97 adds the provider
  execution retry/fallback decorator, and slice 101 consumes that decorator
  through bootstrap's `AgentPromptRunner` for caller-supplied backends while
  the parallel `ToolScheduler` and ordinary CLI/binary handoff with real
  adapters remain downstream. Slice 76
  extended the real `agent::Loop` driver from the slice-75 text-only path into the
  first sequential direct-dispatch tool loop. `<oran/agent.hpp>` exports
  `agent::Loop`, `LoopOptions`, `RunTurnInputs`, and `RunTurnResult`;
  `RunTurnInputs` can now carry optional non-owning `tool::Registry*` and
  `tool::DispatchContext*` pointers. When both are present and the provider
  returns `ToolUseContent`, the loop appends the assistant tool-use message,
  dispatches each tool through the existing registry boundary in original
  tool-use order, appends a `Role::tool` message with ordered
  `ToolResultContent` blocks, rebuilds the seven-section prompt from the
  updated transcript, and sends the next `provider::Request` through the same
  `provider::System` / `provider::Route`. It aggregates provider usage across
  iterations, returns the terminal assistant text/blocks/model id, final
  rendered prompt/cache hints, `iterations`, and the complete transcript tail
  including the final assistant answer. Missing tools and model-repairable
  dispatch errors are converted into `tool_result` error blocks so the model can
  repair; cancellation, storage, and internal dispatch errors propagate out of
  the loop. If the registry/context pair is absent, `tool_use` still returns
  the explicit not-yet-implemented error from slice 75. The loop enforces the
  existing `LoopOptions::max_iterations` cap with `reason=iteration_cap`,
  writes iteration-cap trace rows when tracing is configured, and refreshes
  `DispatchContext::now` around direct dispatch so broker-backed
  `permission_ask_rendered` approvals use the real per-call clock. Provider
  retry/fallback and the parallel `ToolScheduler` remain downstream. `test-agent`
  now covers the FakeProvider text-turn path,
  provider request mapping, provider error forwarding, the no-dispatch-context
  tool-use boundary, one-tool provider re-entry, ordered multi-tool results,
  model-visible missing-tool repair, infrastructure error propagation, and the
  iteration cap, provider/tool cancellation trace rows, and provider/loop-boundary
  error trace rows, and the fake-provider approval-clock path. The
  `orangutan` binary is still not wired to `oran-agent`; remaining near-term
  work is CLI/binary handoff.
  Slice 75 opened
  the real `agent::Loop` driver but deliberately limited it to spec-0017
  scenario #1 and request-mapping boundaries. `<oran/agent.hpp>` began
  exporting `agent::Loop`, `LoopOptions`, `RunTurnInputs`, and
  `RunTurnResult`; the loop owned a `prompt::Builder`, built the seven-section
  prompt from caller-supplied stable inputs and the conversation tail, mapped
  the rendered prefix into `provider::PromptCacheHints`, mirrored the prompt
  active/promoted tool set into deterministically name-sorted
  `provider::Request::tools`, sent one `provider::Request`, and returned
  terminal text-style responses while loudly rejecting `tool_use`.
  Slice 74 closes
  spec 0017's provider prework: `oran-provider` now exports the abstract
  `provider::System` (single `send(Request, Route, EventSink*) const`
  entry), the `provider::EventSink` streaming observer with default no-op
  callbacks for text/thinking/tool deltas plus terminal `on_done`, the
  `ProtocolKind` / `ModelTarget` / `Route` value shapes the loop will
  resolve once per turn, and `provider::FakeProvider` — the first concrete
  `System` — with a `ScriptedTurn` / `StreamDelta` plan, plan-exhaustion
  guard, cancel-aware scripted latency through `async::sleep_for`, and a
  delta-to-`Response` assembler that fans the same calls out to the
  observer. `oran-provider` now depends on `oran-async` (the layer-1
  platform dep already used by `oran-prompt`). `test-provider` covers
  the canned-response path, delta assembly with text+tool blocks, scripted
  error injection, plan exhaustion, empty-turn rejection, multi-turn
  drive, null-sink tolerance, and parent cancellation during scripted
  latency. The provider library is still not linked into the `orangutan`
  binary and does not yet contain a real transport, protocol adapter,
  retry runtime, or vendor cache-control mapping. Remaining near-term
  work at that point was the `agent::Loop` MVP; slice 75 opened the
  text-only subset and slice 76 added sequential direct-dispatch
  provider re-entry, while provider adapter mapping remains downstream. Slice 73
  opens `oran-provider` with the adapter-facing cache-hint surface needed
  between spec 0016 and the fake-provider-first loop. `<oran/provider.hpp>`
  now exports provider-domain `Request`, `Response`, `Usage`, `RetryPolicy`,
  `PromptCacheHints`, `PromptCacheOptions`, and
  `make_prompt_cache_hints(RenderedPrompt, options)`. The mapper validates
  the prompt-design boundary (`RenderedPrompt::sections` has exactly seven
  sections, exactly one breakpoint, and that breakpoint is section 6 before
  the conversation tail), checks `prefix_bytes` against the actual section
  bytes, maps sections 1-6 into `(id, content_hash, cache_version)` cache
  keys plus the prefix hash/byte count, excludes `conversation_tail`, and
  supports route-level cache disable / minimum-prefix skip. `test-provider`
  covers successful prefix-only mapping, disable/size-floor skips, and
  malformed boundary rejection; `bench-provider` compares
  `provider.cache_hints_enabled` at about 394 ns / mapping with the disabled
  route at about 317 ns / mapping. Slice 73's surface is the prerequisite the
  slice-74 fake-provider foundation consumes; together they enabled the
  slice-75 text-turn `agent::Loop` foundation; slice 76 consumes the same
  provider contract for the first sequential tool-dispatch scenario matrix.
  Slice 72
  opens `oran-agent` with the narrow session-state owner needed by spec
  0016 before the full ReAct loop lands. `agent::SessionState` owns
  `prompt::PromotionState`, observes successful `tool.search` outputs,
  parses their structured `{kind:"tool_search", matches[]}` payload in a
  private `nlohmann_json` TU, promotes only deferred match names into the
  next prompt snapshot, ignores non-search and failed-search outputs, and
  returns `ErrorKind::invalid_argument` for malformed successful structured
  data without mutating state. `test-agent` covers promotion into the next
  prompt, no-op non-search / failed-search outputs, and malformed successful
  payload rejection; `bench-agent` now runs the agent-owned prompt-cache
  fixture (`agent.prompt_cache_no_promotions` about 54.4 us / fixture,
  `agent.prompt_cache_after_promotion` about 63.1 us / fixture) and aborts
  if `RenderedPrompt::prefix_hash` drifts across changing conversation tails.
  `oran-agent` is not linked into the `orangutan` binary yet and does not
  contain the fake-provider ReAct loop. Slice 71
  extended `oran-prompt` with `prompt::PromotionState`, a session-owned
  value type for deferred-tool promotions, and taught `prompt::Builder` to
  consume sorted promotion snapshots. Slice 70 opened the `prompt::Builder`
  skeleton, slice 69 landed the typed `runtime.prompt.active_tools` config
  surface, and slice 68 landed the registry-owned non-deferred
  `tool.search` lookup primitive.
  Slice 67
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
  builder or promotion-set slice; slice 68 adds the registry-local
  `tool.search` lookup primitive that this renderer's future prompt
  builder will advertise as an active tool, and slice 69 adds the typed
  config surface that will select the active set.
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
  first; binding the CLI approval sink into real turns is still gated on
  the provider-backed `oran-agent` handoff; and wiring
  `check-compile-budget.sh` into
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
| **C** | Compile-time discipline, Tests, Benches, IO, Storage, Config, Bootstrap, Provider system, Tool registry, Prompt builder, Memory tiers, Permissions, Hooks, Channels, Orchestration, Automation, Web UI, CLI, Static analysis |
| **D** | Skills, Observability |

## Latest Library Surfaces

- `oran-core`: 71 cases / 455 assertions.
- `oran-async`: 9 cases / 43 assertions.
- `oran-io`: 49 cases / 286 assertions.
- `oran-storage`: 72 cases / 899 assertions.
- `oran-config`: 33 cases / 241 assertions.
- `oran-permission`: 89 cases / 426 assertions.
- `oran-hook`: 30 cases / 207 assertions.
- `oran-tool`: 178 cases / 1838 assertions.
- `oran-prompt`: 10 cases / 98 assertions.
- `oran-provider`: 57 cases / 442 assertions.
- `oran-agent`: 25 cases / 401 assertions.
- `oran-cli`: 14 cases / 97 assertions.
- `oran-bootstrap`: 65 cases / 269 assertions.

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
- 2026-05-17 — `file.search` does not yet ship ripgrep-class optimisations
  (mmap, extension-based binary skip, multi-threaded walk).
  Adequate at slice 20 (~27 µs / 4-file tree) but 3-10× slower than a tuned
  scanner on repo-scale inputs. Re-bench once `oran-agent` produces a real
  workload measurement.
- 2026-05-17 — `scripts/check-prompt-preamble` static grep promised in
  `rules/prompt-design.md` not yet implemented (waits on first stable
  preamble template in `oran-agent`).
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
