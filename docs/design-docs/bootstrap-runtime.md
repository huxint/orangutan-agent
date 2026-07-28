# Bootstrap Runtime

`oran-bootstrap` is the process entry boundary. It owns command-line bootstrap
parsing, config discovery, and runtime assembly for the services the binary can
construct before handing prompts to the provider-backed agent loop.

## Current Public API

```cpp
namespace orangutan::bootstrap {

enum class ConfigSource {
  built_in_defaults,
  default_file,
  explicit_file,
};

struct BootstrapOptions {
  std::span<const std::string_view> args;
  std::string workspace = ".";
};

struct LoadedConfig {
  config::Config value;
  ConfigSource source;
  std::string path;
};

core::Result<LoadedConfig> load_config(BootstrapOptions);
core::Result<int> run(BootstrapOptions);

struct AgentPromptRunnerOptions;
struct AutomationAgentPromptRunnerOptions;
struct ChannelAgentPromptRunnerOptions;
struct ChannelRegistrationReport;

core::Result<automation::AutomationPromptRunner>
make_automation_agent_prompt_runner(AutomationAgentPromptRunnerOptions);

core::Result<ChannelRegistrationReport>
register_configured_channels(channel::ChannelManager&, asio::any_io_executor, const config::Config&);

core::Result<channel::ChannelPromptRunner>
make_routed_channel_prompt_runner(ChannelAgentPromptRunnerOptions);

class AgentPromptRunner final : public cli::PromptRunner {
 public:
  static core::Result<std::unique_ptr<AgentPromptRunner>>
  create(AgentPromptRunnerOptions);

  async::Awaitable<core::Result<cli::PromptRunResult>>
  run_prompt(cli::PromptRunRequest) override;
};

}  // namespace orangutan::bootstrap
```

All public functions return `core::Result<T>`. `src/main.cpp` only builds the argument
span, calls `bootstrap::run`, and converts an error into process exit code `1`.
`bootstrap::run` consumes bootstrap-owned flags, loads config, and forwards remaining
CLI args to `cli::run` when no provider route is configured, or to `cli::run_async`
with a bootstrap-owned `AgentPromptRunner` when config declares a provider route.

`<oran/bootstrap/runtime_assembly.hpp>` also exposes the per-process service bundle:

```cpp
namespace orangutan::bootstrap {

struct LongtermMemoryStartupDecayOptions {
  std::string scope_key{};
  core::Time unused_before{core::Time::epoch()};
  double importance_floor{0.0};
  std::size_t limit{0};
  core::Time decay_at{core::Time::epoch()};
};

struct RuntimeStartupHookBinding {
  hook::Sink* sink{nullptr};
  std::vector<hook::Event> events{};
};

struct RuntimeAssemblyOptions {
  std::string audit_db_path{};
  bool audit_enabled = true;
  std::size_t audit_reader_count{1};
  std::size_t audit_statement_cache_capacity{4};
  tool::WorkspaceOptions workspace_options;
  bool trace_enabled = true;
  std::optional<std::int64_t> trace_retention_started_before_ns{};
  std::chrono::milliseconds hook_blocking_timeout{2000};
  std::vector<RuntimeStartupHookBinding> startup_hook_bindings{};
  std::string sessions_db_path{};
  bool session_memory_enabled = true;
  std::size_t session_reader_count{2};
  std::size_t session_statement_cache_capacity{8};
  std::string longterm_memory_db_path{};
  bool longterm_memory_enabled = true;
  std::size_t longterm_memory_reader_count{2};
  std::size_t longterm_memory_statement_cache_capacity{16};
  std::optional<LongtermMemoryStartupDecayOptions> longterm_memory_startup_decay{};
  std::optional<automation::MemoryRetentionJob> longterm_memory_retention_job{};
  std::vector<automation::UpsertCronJobRequest> cron_jobs{};
  std::vector<automation::UpsertTriggeredJobRequest> triggered_jobs{};
  bool longterm_vector_memory_enabled = false;
  std::string longterm_vector_memory_db_path{};
  std::size_t longterm_vector_memory_dimensions{64};
  std::size_t longterm_vector_memory_reader_count{2};
  std::size_t longterm_vector_memory_statement_cache_capacity{16};
};

class RuntimeAssembly {
 public:
  static core::Result<RuntimeAssembly> build(
      std::string_view workspace,
      asio::any_io_executor runtime_executor,
      RuntimeAssemblyOptions options = {});

  permission::ApprovalBroker& approval_broker() noexcept;
  permission::AuditSink& audit_sink() noexcept;
  tool::Workspace& workspace() noexcept;
  storage::TraceRepository* trace_repository() noexcept;
  bool trace_enabled() const noexcept;
  hook::Bus& hook_bus() noexcept;
  memory::session::Store* session_store() noexcept;
  bool session_memory_enabled() const noexcept;
  std::string_view sessions_path() const noexcept;
  memory::longterm::Backend* longterm_memory_backend() noexcept;
  memory::longterm::Runtime* longterm_memory_runtime() noexcept;
  memory::longterm::VectorBackend* longterm_vector_backend() noexcept;
  memory::longterm::HybridRuntime* longterm_hybrid_runtime() noexcept;
  bool longterm_memory_enabled() const noexcept;
  std::optional<std::size_t>
  longterm_memory_startup_decay_shadowed_count() const noexcept;
  const std::optional<automation::MemoryRetentionJob>&
  longterm_memory_retention_job() const noexcept;
  const std::vector<automation::UpsertCronJobRequest>&
  cron_jobs() const noexcept;
  const std::vector<automation::UpsertTriggeredJobRequest>&
  triggered_jobs() const noexcept;
  bool longterm_vector_memory_enabled() const noexcept;
  std::string_view longterm_memory_path() const noexcept;
  std::string_view longterm_vector_memory_path() const noexcept;
};

}  // namespace orangutan::bootstrap
```

The assembly owns the approval broker, audit sink/repository pool, workspace resolver,
optional trace repository, process hook bus, build-only startup hook bindings,
optional session-memory store, and optional long-term memory backend/runtime.
Session memory uses a separate `storage::Pool` and `storage::SessionRepository` over
`<workspace>/.orangutan/sessions.db` by default; it never shares the audit/trace
`audit.db` pool. Long-term memory uses a separate `storage::Pool` over
`<workspace>/.orangutan/memory.db`, migrates the `memory::longterm::Fts5Backend`
schema before opening the long-lived pool, optionally runs one bounded startup
decay pass from `LongtermMemoryStartupDecayOptions`, and exposes both the backend
and `memory::longterm::Runtime` for prompt-boundary recall and the read-only
`MemoryRecall` tool plus the write-side `MemoryRemember` and `MemoryForget`
tools. When vector memory is enabled, the assembly opens a separate
`<workspace>/.orangutan/memory-vectors.db` pool with sqlite-vec auto extensions,
migrates `memory::longterm::SqliteVecBackend`, and exposes both the vector
backend and `memory::longterm::HybridRuntime`.
`bootstrap::run` threads `config.trace().enabled` into `trace_enabled`, converts
`config.trace().retention_days` into an explicit Unix-nanosecond cutoff for
`trace_retention_started_before_ns`, `config.hooks().timeout_ms` into
`hook_blocking_timeout`, and enables session plus long-term memory only when a
provider route is configured. When that cutoff is present and tracing is enabled,
`RuntimeAssembly::build` purges old `trace_turns` rows after the audit migration
and before the long-lived trace repository is exposed; audit rows are not deleted
by trace retention. For configured-route runs, bootstrap also maps
`memory.longterm.retention` for the runner's stable `cli` scope into an
automation-owned `MemoryRetentionJob` descriptor plus one
`longterm_memory_startup_decay` pass derived from the same descriptor. The
descriptor's `first_fire_at` is the startup decay clock plus
`decay_check_interval_hours`, so future periodic ownership starts after the
startup pass instead of immediately repeating it. The assembly applies the
one-shot lexical-memory pass after long-term migration and before the long-lived
memory pool is exposed, so prompt and tool reads see the post-decay visible set.
After the pass succeeds, the assembly publishes advisory `memory_decay` with
metadata for the startup source, scope, retention policy inputs, shadowed count,
and timing; decayed record content is not included. It stores the pass result on
the assembly as `longterm_memory_startup_decay_shadowed_count()`: `std::nullopt`
means no startup pass was configured or run, while `0` or higher means the pass
ran and reports how many records were shadowed. It also stores the periodic seed
as `longterm_memory_retention_job()` for diagnostics and future scheduler
ownership. Bootstrap also maps `automation.cron.jobs[]` and
`automation.triggered.jobs[]` into automation repository seed descriptors and
stores them as `cron_jobs()` / `triggered_jobs()` for diagnostics and later
runtime owners. `RuntimeAssembly::build` does not evaluate, persist, lease, run,
enqueue, or execute any of those automation descriptors; it is not a startup-loop
timer.
`startup_hook_bindings` are installed immediately after the bus is constructed
and before startup producers run; null sinks are rejected with
`reason=null_sink`, and every startup-only observer is unbound before the
assembly is returned. Regular long-lived observers still bind through
`hook_bus()` after `build()` returns for prompt/tool/provider lifecycle events.
The
built-in empty-defaults path disables session and long-term memory so a fresh
checkout can run the deterministic CLI shell without opening `sessions.db` or
`memory.db`. The startup banner prints
`trace=<enabled|disabled>`, `sessions=<enabled|disabled> (<path|disabled>)`,
`longterm-memory=<enabled|disabled> (<path|disabled>)`,
`startup-decay=<disabled|N>`,
`vector-memory=<enabled|disabled> (<path|disabled>)`, `hook-timeout=<ms>`,
`automation-cron-jobs=<N>`, and `automation-triggered-jobs=<N>`.

`<oran/bootstrap/prompt_runner.hpp>` exposes the bootstrap-owned CLI runner used by
tests and the ordinary binary handoff for configured routes. `AgentPromptRunner::create`
borrows a caller-supplied `RuntimeAssembly`, typed config, resolved `provider::Route`,
executor, and provider backend; registers the builtin tool catalog; materializes
permission rules from config plus an optional agent overlay; wraps the backend in
`provider::execution::Runtime`; binds `cli::OperatorPromptSink` to the
assembly-owned `permission_ask_rendered` bus by default; and drives `agent::Loop`
with workspace, audit, broker, hook, output-cap, trace, and session-memory
services from the assembly. `AgentPromptRunnerOptions::bind_operator_prompt_sink`
lets noninteractive callers disable that binding so `Verdict::ask` stays on the
existing fail-closed no-sink path instead of reading terminal stdin.
When the assembly exposes `session_store()`, the runner loads the persisted history for
the current `session_id` + `agent_key` plus the durable skill activation rows for
that same key before each prompt, then appends only the new successful transcript
suffix and records successful skill activation/deactivation updates afterward. The
in-process transcript cache remains the fallback when session memory is disabled. The
runner also owns an
`agent::SystemPreambleOwner` for prompt section 1 and a `memory::FramingOwner`
for prompt section 5, rendering both exactly once before calling `agent::Loop`.
It also owns the section-4 `skill::CatalogOwner`: callers can provide exact
`AgentPromptRunnerOptions::skills_catalog` bytes, or configured-route startup can
point `skills_directory` at `<workspace>/.orangutan/skills` so the runner owns a
`skill::WorkspaceSkillSnapshot`. The snapshot loads before the first prompt and
refreshes at later prompt boundaries after watcher/signature changes, replacing
the rendered catalog and loaded `SkillDocument` vector together. Missing skills
directories produce an empty catalog, loader errors fail before the loop, and
`skill_catalog_loads()` exposes the number of snapshot reloads. Before rendering
section 4, the runner asks `oran-skill` to resolve active skill markers through
the current `skill::ActivationPolicy`. That policy currently derives markers
from the already-loaded conversation transcript, nets successful `SkillInvoke`
activation and `SkillDeactivate` deactivation tool-results that carry the
versioned `data_json` records in transcript order (most recent event wins), overlays
durable session activation rows loaded from `memory::session::Store`, and filters
through the current loaded/allowed catalog entries so removed or disallowed skills do
not remain active in the next prompt. The policy surface also accepts explicit
deactivated skill names plus explicit expiration rows with caller-supplied evaluation
time. The configured-route runner now sources those from the selected agent config: it reads
`agents.<name>.skills_deactivated` into `ActivationPolicy::deactivated_skill_names`,
maps each `agents.<name>.skills_expirations` `config::SkillExpirationConfig`
entry to a `skill::SkillExpiration`, and supplies
`evaluation_time = core::time::now_utc()` at the prompt boundary only when
expirations are present, so the section-4 renderer never reads a clock.
Default and no-agent callers still resolve an empty policy, so unchanged
configurations behave exactly as before. Automatic policy resolution runs only
when the runner owns the workspace skill snapshot; exact
`AgentPromptRunnerOptions::skills_catalog` bytes are treated as authoritative
pre-rendered section-4 text for tests/embedders and bypass loader refresh plus
activation-policy rendering. Policy output is applied before `agent::Loop`
starts, never mid-turn, so multi-iteration provider/tool loops reuse one
section-4 prefix while skill bodies continue to arrive only as conversation-tail
tool-result text.
Slice 164 adds the equivalent opt-in path for long-term memory recall:
`AgentPromptRunnerOptions::longterm_recall` defaults disabled, and when enabled
requires an assembly-owned `memory::longterm::Runtime`, a positive limit, and no
exact `AgentPromptRunnerOptions::memory_framing` override. The runner derives a
`longterm::Query` from the configured query strategy plus its stable
`scope_key`, calls `Runtime::recall(...)` once before the user prompt enters the
conversation tail, then renders section 5 from the returned records.
Multi-iteration provider/tool loops reuse those same bytes for the turn. Slice
165 adds the ordinary
configured-route policy source: `memory.longterm.recall.enabled` and
`memory.longterm.recall.limit` parse through `oran-config`, default to disabled
and `5`, and `bootstrap::run` maps them into the runner option after building
the provider-backed runtime assembly. Slice 166 extends the same policy with
optional `memory.longterm.recall.kinds`; bootstrap validates those strings
against `memory::longterm::RecordKind` and passes the parsed filter to the
prompt-boundary query. Omitting `kinds` preserves all-kind recall. Slice 167 adds
`memory.longterm.recall.query_strategy`: `prompt_text` preserves the current
prompt text query, while `last_user_message` uses the latest prior user text
when one exists so follow-up prompts can recall from session context.
Slice 168 also installs `DispatchContext::memory_recall` for the runner-owned
tool registry. The `MemoryRecall` built-in parses and gates the call in
`oran-tool`, then bootstrap adapts the request to the assembly-owned
`memory::longterm::Runtime` with the runner's stable `scope_key`; successful
tool results return deterministic recall text, structured `memory_recall`
record metadata, and `usage.match_count` through the normal provider re-entry
path.
Slice 178 consumes `memory.longterm.hybrid_search.enabled` in configured-route
startup when the binary is built with `--vector_memory=y`: bootstrap enables the
assembly-owned vector pool/backend, maps the configured hybrid limits/weights
into `AgentPromptRunnerOptions::longterm_hybrid_search`, and the runner uses a
deterministic local text embedding owner for prompt/query embeddings. Prompt
boundary recall and `MemoryRecall` use `HybridRuntime`; `MemoryRemember` and
`MemoryForget` mirror vector upserts/deletes when a vector backend is present.
Default builds still reject `enabled=true` before `RuntimeAssembly::build` or
provider construction with `ErrorKind::config`,
`path=$.memory.longterm.hybrid_search.enabled`,
`reason=build_option_disabled`, and `option=vector_memory`.
Slice 169 installs `DispatchContext::memory_remember` beside that recall
binding. The `MemoryRemember` built-in parses and gates one record-shaped write
in `oran-tool`; bootstrap adapts it to the assembly-owned
`memory::longterm::Backend`, stamps the runner's stable `scope_key` plus
`DispatchContext::now` timestamps, publishes blocking `memory_write_before`
before mutating the backend, and returns confirmation text, structured
`memory_remember` saved-record metadata, and `usage.bytes_written` through the
same provider re-entry path. Slice 179 makes that pre-write hook a real runtime
gate: `proceed` continues, `veto` returns a permission-denied tool error with
`reason=blocked_by_hook` and skips lexical/vector writes, and
`rewrite` / `require_approval` are rejected as unsupported for this memory
consumer. After a successful lexical/vector write, the runner publishes
advisory `memory_write_after` with the saved record.
Slice 170 installs `DispatchContext::memory_forget` beside those memory
bindings. The `MemoryForget` built-in parses and gates one `{id}` delete in
`oran-tool`; bootstrap adapts it to the assembly-owned
`memory::longterm::Backend`, supplies the runner's stable `scope_key`, calls the
backend's idempotent `remove(...)`, and returns confirmation text, structured
`memory_forget` removed-key metadata, and `usage.bytes_written = 0` through the
same provider re-entry path. Slice 179 publishes advisory `memory_forget` after
that successful scoped delete. Slice 180 publishes advisory `memory_read_after`
after successful prompt-boundary long-term recall and after successful
`MemoryRecall` tool reads, for both lexical and hybrid recall paths. Memory
lifecycle payloads carry runner identity, scope/id/source metadata, timing
fields, and per-sink redaction: default hook sinks receive redacted summaries
for writes and reads, while `trusted_local` sinks receive raw long-term memory
records and recall queries.
When the caller
selects an `AgentPromptRunnerOptions::agent_config_name` (or leaves it empty
and selects `permission_agent_name`), the runner reads that `agents.<name>`
entry's optional `prompt_overlay` and `skills_enabled` fields before prompt
execution. `prompt_overlay` fills stable prompt section 6 when callers did not
provide exact `AgentPromptRunnerOptions::per_agent_overlay` bytes. An absent
skill allowlist keeps all loaded skills visible; a present allowlist,
including an empty array, filters both the section-4 catalog and the invocation
document vector after each workspace snapshot refresh. Ordinary
configured-route binary startup now maps `--agent <name>` into both
`agent_config_name` and `permission_agent_name`, so the same operator selector
controls permission overlay, prompt overlay, skill allowlist, audit/session
agent key, and provider hook metadata.
The same filtered document vector is the immutable source for the
`SkillInvoke` built-in during that runner's current turn: bootstrap installs a
`DispatchContext::skill_invoke` callback, and the tool dispatch path returns the
matched markdown body as ordinary tool-result text for the next provider
iteration without re-reading disk mid-turn. Successful invokes also return a
small structured activation record through `Output::data_json`; the next prompt
boundary feeds that record through `skill::resolve_active_skills(...)` to mark
the skill active in section 4 without parsing model-visible body text.
Slice 147 installs a parallel `DispatchContext::skill_deactivate` callback over
the same filtered document vector: `SkillDeactivate` returns a versioned
`skill_deactivation` record in `Output::data_json`, and the next prompt boundary
nets it against prior activations through the same transcript scan so a
mid-session deactivation clears the section-4 marker without a config edit or a
renderer clock. Slice 148 persists successful `SkillInvoke` / `SkillDeactivate`
results as per-session activation rows after the turn succeeds. Those rows overlay
the transcript-derived set on later prompts, so pruning old transcript tool results
cannot lose the latest active/inactive decision. Slice 149 moves the transcript
activation/deactivation event scan behind
`skill::skill_activation_events_from_transcript(...)`; bootstrap now maps those
semantic `skill::SkillActivationEvent` rows into
`memory::session::SkillActivationUpdate` instead of owning another
skill-result parser. Other runtime entry points can therefore persist the same
event stream without changing the prompt-builder boundary.
Empty `AgentPromptRunnerOptions::system_preamble` selects
`agent::default_system_preamble()`; explicit text is treated as an override for
tests and embedders. Multi-iteration provider/tool turns therefore reuse stable
system-preamble and memory-framing bytes without moving section rendering into
the loop. After every successful turn the runner walks
the new transcript suffix and feeds each tool result back through
`agent::SessionState::observe_tool_output(...)` so the next turn's
`promotion_snapshot(...)` sees deferred-tool promotions; the count of observed
`ToolSearch` results is exposed for diagnostics through
`tool_search_observations_recorded()`, and section render counters are exposed
through `system_preamble_renders()` and `memory_framing_renders()`. The runner
does not construct real provider adapters or read provider credentials.

`<oran/bootstrap/automation_prompt_runner.hpp>` exposes the first bootstrap-owned
automation bridge above the automation library's injected prompt seam.
`make_automation_agent_prompt_runner(...)` returns an
`automation::AutomationPromptRunner` that validates the supplied runtime
assembly, config, provider backend, route, scope, and identity, then constructs
one `AgentPromptRunner` per durable automation job execution. The bridge
derives a stable per-job `session_id` from job type, scope key, durable job
key, and agent key so session-memory-backed jobs reuse the same transcript
state across later executions. It applies `agents.<name>` prompt and permission
overlays only when that config entry exists, leaves them empty otherwise, and
defaults `bind_operator_prompt_sink=false` so automation asks stay fail-closed.
The bridge does not open `automation.db`, persist cron seeds, start timers, or
launch background work; callers still own `AutomationRuntime`, cron service
cycles, and triggered execution.

## Config Resolution

`run` and `load_config` consume these bootstrap arguments:

- `--config <path>`
- `--config=<path>`
- `--help` / `-h`

`xmake run` may pass a standalone `--` separator through to the binary; bootstrap
accepts and ignores that separator so repo smoke commands can use the usual
`xmake run orangutan -- --config config.example.json` form.

Arguments not owned by bootstrap are left for `oran-cli`. For example,
`--prompt <text>` is not used during config resolution; after config loading,
`bootstrap::run` forwards it to `cli::run`.

When `--config` is supplied, that file is required. Missing, unreadable, or invalid
explicit config returns a `core::Result` error.

When `--config` is not supplied, bootstrap resolves:

```text
<workspace>/.orangutan/config.json
```

If the default file exists, bootstrap loads it through `oran-config`. If it is absent,
the current early runtime uses `config::Config{}` built-in defaults and reports
`ConfigSource::built_in_defaults`. This keeps a fresh checkout runnable before
`oran-bootstrap` owns full workspace initialization.

## Binary Output

The current `orangutan` binary prints:

- version / slice banner,
- config source and path,
- profile / route / worker / web summary,
- default provider-route summary when config declares routes (or
  `provider route: none configured` for the built-in empty defaults),
- runtime assembly summary including audit path, workspace root, trace state, and
  sessions state/path plus blocking hook timeout,
- the `oran-cli` mode output.

When config declares routes, bootstrap resolves the `default` route through
`provider::resolve_route_profiles`, builds
`provider::make_adapter_construction_plan`, constructs `HttpProviderBackend`,
creates `AgentPromptRunner`, and runs the remaining CLI args through
`cli::run_async`. The existing `--mode
<strict|default|permissive|sandboxed>` selector chooses the runner's permission
baseline, and `--agent <name>` selects the matching `agents.<name>` entry for
permission overlays, skill allowlists, and runtime agent identity. Unknown
agent names fail before prompt execution through the runner's existing
not-found validation. No-prompt configured-route runs set
`CliOptions::interactive_repl`, so `oran-cli` reads terminal prompts until an
empty line, `/exit`, `/quit`, or EOF and dispatches each non-command prompt
through that runner. `/help` is handled locally by `oran-cli` and never reaches
`AgentPromptRunner`. The route/plan
preflight catches bad profile references,
provider labels, explicit profile-protocol spellings, missing endpoint
metadata, and unsupported endpoint schemes before credentials are read.
Resolved route targets also carry optional non-secret provider pricing from
`profiles.<name>.pricing`; `agent::Loop` uses it to compute a missing provider
usage cost estimate before lifecycle hooks and trace rows observe the turn.
`HttpProviderBackend::build` then crosses the credential and
adapter-construction boundary: it resolves the configured API-key environment
variables, owns an `http::Client`, adapts it to
`provider::ProtocolTransport`, registers the built-in Anthropic/OpenAI protocol
factories, and returns a `provider::System` plus route for the runner.
Configured-route prompts now start `agent::Loop` from the ordinary binary path;
missing credentials fail as `ErrorKind::auth` with only non-secret context.
`AgentPromptRunner` also threads the assembly-owned hook bus plus scope/agent
identity metadata into every loop turn, so provider request/response/error and
fallback hooks are published for configured-route prompts without making
`oran-provider` depend on the hook subsystem.
The same configured-route provider backend, route, config, and runtime assembly
can also be handed to `make_automation_agent_prompt_runner(...)` when a caller
opens `AutomationRuntime` explicitly and wants stored cron or triggered
`agent_prompt` descriptors to execute through the real prompt runtime one job
at a time. Bootstrap still does not do that automatically during ordinary
startup.
Channel runtime owners use the parallel channel ingress seam:
`register_configured_channels(manager, executor, config)` constructs configured
adapters into a caller-owned `channel::ChannelManager`, while
`make_routed_channel_prompt_runner(...)` routes each configured channel id to
its selected agent bridge. The registration function is construction-only: it
does not call `Channel::start()`, does not spawn receive loops, and does not
drive `dispatch_one(...)`. For `kind == "mock"` it returns non-owning mock
handles for loopback tests. For `kind == "qq"`, default `--channel_qq=n` builds
skip and report the entry without linking `oran-channel-qq`; enabled builds
resolve `qq_app_id_env` / `qq_client_secret_env` at the credential boundary,
assemble `http::Client`, `qq::TokenStore`, `qq::ApiClient`,
`qq::GatewayTransport`, and `qq::QqChannel` behind a private owning wrapper,
then register that wrapper as a generic `Channel`. The QQ branch requires a
configured `qq_gateway_url` until milestone 4 owns gateway discovery and
round-trip acceptance.
The built-in empty-defaults path still reports `provider route: none configured`
and uses the deterministic no-runner `cli::run` shell so fresh checkouts remain
runnable without provider credentials or a sessions DB. In that mode CLI prompt
output says the prompt was not sent because no provider route is configured;
configured-route prompts already run through `AgentPromptRunner` and
`agent::Loop`. Selector flags
(`--mode` / `--agent`) are rejected on that path unless `--explain-rules` is
active. The runtime assembly opens the audit DB when audit is enabled so
migrations, trace repository ownership, and audit sinks are available before
configured-route prompt execution. Configured provider routes additionally open
and migrate the separate sessions DB and expose the typed
`memory::session::Store` for the runner persistence slice.

`orangutan --trace <turn-id>` is a bootstrap-owned one-shot inspector. It
opens the workspace audit DB, runs the idempotent audit migration, loads the
`trace_turns` row, then prints joined `audit_events` rows ordered by `id ASC`.
Since slice 93 those audit lines include `kind=<event_kind>`, so mixed
permission-decision and `hook_publish` rows are readable in the same output.
Slice 242 extends the sibling `orangutan --trace-export` path: with a
`<turn-id>` it reuses the same read-only lookup and emits one JSON Lines object
with the trace row plus ordered joined audit rows; without a turn id it lists
newest turns through `TraceRepository::list_turns`, applies optional
`--agent <name>` and positive `--limit <n>` filters, joins each turn's audit
rows, and emits one JSON Lines object per turn. Adding
`--trace-export-file <path>` to either export form writes the same JSON Lines
sequence to an explicit file sink, creates parent directories, truncates the
target file, and suppresses stdout. Adding `--trace-export-post <url>` instead
uses `oran-http::Client` to POST the same newline-delimited JSON payload as
`application/x-ndjson` to an explicit operator URL, suppresses stdout, accepts
2xx responses, and returns an IO error for non-2xx responses with the status
code. The trace DB lookup still shares the same idempotent migration and
signal-aware one-shot drain, while single-turn export keeps the `--trace`
turn-id validation and missing-row behavior.

## Service Mode (`--serve`)

`orangutan --serve` is the long-lived runtime-service owner — a mode of the main
binary, parallel to `--desktop`, **not** a separate `orangutan-server` (see the
[runtime-service-owner plan](../exec-plans/completed/2026-06-18-runtime-service-owner.md)).
It exists to drive the periodic concerns that `oran-automation`, `oran-io`, and the
tool scheduler were built to expect but which nothing started.

`bootstrap::run_serve` (`src/oran-bootstrap/serve.cpp`):

1. loads config and builds `async::Runtime`;
2. creates a runtime strand, an `asio::cancellation_signal`, and an
   `asio::signal_set{strand, SIGINT, SIGTERM}` whose handler records the signum and
   emits `cancellation_type::terminal` on that signal — the handler and the service
   coroutine share the strand, so the cross-thread emit is serialized with slot
   consumption regardless of io-worker count;
3. co-spawns the service body (the watcher concern always, plus automation,
   scheduler reaping, and channel concerns when their config gates are present
   — see below) bound to the cancellation slot, with a completion handler that
   fulfils a `std::promise<core::Result<void>>`;
4. `Runtime::start()`s (non-blocking), then blocks the calling thread on the
   completion future until a signal stops the service;
5. on completion, `Runtime::stop_and_join()`s and returns a `cancelled` error carrying
   `signal`/`signum`, which `bootstrap::run` maps to `128 + signum` (the same seam
   as `--audit-init`/`--trace`).

`serve_run(executor, ServeOptions)` is the file-view **watcher** concern — drivable
from tests with a plain `io_context` + `cancellation_signal` (no real process or
signal). It runs the watcher and idles until its cancellation slot fires, returning
`core::Result<void>{}` (a graceful stop is not an error). `serve_automation(executor,
service, cron_handler, triggered_handler, options, stop_requested)` is the **automation**
concern: it drives `automation::AutomationService::run` in a cancel-aware poll loop,
firing any cron job due at the current UTC minute and draining buffered triggered
work each tick. `serve_scheduler_reaping(executor, scheduler, options, stop_requested)`
is the **scheduler idle-lock reaping** concern: a cancel-aware loop that periodically
calls `agent::ToolScheduler::reap_idle_locks(now)` to bound the shared scheduler's
per-path lock table (spec 0012 AC10). `serve_channels(executor, manager, runner,
channel_ids, stop_requested, triggered_service, options)` is the **channel ingress/dispatch**
concern: it drives already-started configured adapters by spawning one pump per
adapter into the manager fan-in, assigning messages to bounded
per-channel+conversation worker queues, dispatching each conversation in order
through the routed agent bridge while unrelated conversations can run
concurrently, evicting empty conversation workers after their idle TTL, publishing
structured worker metrics snapshots to an optional C++ observer and the default
daemon stderr sink, applying an optional per-message deadline with a still-working fallback reply,
optionally enqueueing `channel:<channel_id>` triggered automation work when
`triggered_service` is supplied, replying through the owning adapter, and
stopping/draining adapters before returning. A
file-local `serve_body` races the enabled concerns with the awaitable-operators `||`
under one cancellation slot, so a single signal stops all of them. The whole thing is built on a fine-grained
`asio::cancellation_signal` rather than `SignalScope`/`io.stop()` deliberately, so the
automation loop's SQLite writes and in-flight agent turns are never blunt-dropped —
the evolution `signal_drain.hpp` anticipates.

**Slice A (slice 253)** shipped the lifecycle plus the IO file-view cache watcher
(`io::watch_read_text_file_ranged_cache`, `max_events = 0` = run-until-cancelled). A
watcher that cannot initialize (e.g. inotify unavailable, a missing root) is
non-fatal — reported once, then the service keeps idling until signalled.

**Slice B (slice 254, extended by slices 257-258)** adds the automation
cron/triggered loop. The presence of config-authored `automation.cron.jobs[]` or
`automation.triggered.jobs[]` gates it: with neither, `--serve` is exactly the
slice-A watcher (no provider, no `automation.db`, CI-identical). With automation jobs,
`run_serve` builds a `RuntimeAssembly` and a provider — `HttpProviderBackend` for a
configured `default` route, otherwise an offline scripted `FakeProvider` so the loop
stays usable without credentials (the same offline posture as `--desktop`) — then
`serve_body` opens `<workspace>/.orangutan/automation.db` (`AutomationRuntime::open`),
applies the mapped seeds once (`cron_jobs_from` → `apply_cron_job_seeds`, then
`triggered_jobs_from` → `apply_triggered_job_seeds`; applying cron seeds once, not per
tick, is what keeps stored `last_fired_at` from resetting), builds a prompt-backed
handler (`make_automation_agent_prompt_runner` →
`make_cron_prompt_handler` / `make_triggered_prompt_handler`), and races
`serve_automation` beside the watcher. A database that cannot open, or seeds that
cannot apply, is non-fatal — reported once, then the service serves the watcher
alone. Slice 258 adds the first external producer for those durable triggered
descriptors: when configured channels are also active, `serve_channels` wraps the
routed channel prompt runner and calls `AutomationService::enqueue_triggered(...)`
with trigger key `channel:<channel_id>` before the direct channel reply path runs.
The enqueue path is report-and-continue, so a queue/repository failure does not
block the user-visible channel reply. Slice 268 adds the automation-side
`WebhookProducer` seam plus triggered payload propagation, and slice 269 binds it
to `--serve`: `automation.webhooks.listener` can enable a narrow loopback HTTP
listener for `POST <path_prefix><id>`, which preserves a bounded
`Content-Length` body as triggered payload bytes and feeds the same
`AutomationService` queue through `WebhookProducer`. Slice 273 rejects
wildcard/public `bind_host` values before opening the acceptor because this
first listener has no authentication surface.
The automation service disables cancellation
around its durable lease/run-row writes, so a firing tick can swallow a parent cancellation; `serve_automation`'s
`stop_requested` predicate (tied to the trapped signum) is therefore the
authoritative, guaranteed stop, checked before and after each tick, and `run_serve`
always supplies it.

**Slice C (slice 255)** adds the tool-scheduler idle-lock reaping tick and the
ownership hoist it required. `AgentPromptRunnerOptions` gained an optional
`{registry, scheduler}` pair (both-or-neither): when set, the runner borrows them
instead of building its own, so a long-lived owner can share one
`agent::ToolScheduler` across many short-lived per-job runners. When automation is
enabled, `run_serve` builds one `tool::Registry` + `agent::ToolScheduler` on its
stack, drives them on the **runtime strand** (not the multi-worker
`runtime.executor()`), and injects them into `make_automation_agent_prompt_runner`;
the per-job runners then accumulate path locks into that one shared table, and
`serve_body` races `serve_scheduler_reaping` beside the watcher and automation loop to
sweep it on the same strand. The single-strand choice is deliberate: the scheduler's
lock table is single-strand by contract (`src/oran-agent/_impl/path_lock_table.hpp`),
so reaping must not race in-flight dispatch — sharing the strand satisfies that and
also corrects the slice-B per-job scheduler, which ran on the multi-worker executor.
Reaping is a synchronous in-memory sweep that never disables cancellation, so unlike
the automation tick it cannot swallow a parent cancellation; the `stop_requested`
predicate is still honored for symmetry.

**Slice 256** adds the configured-channel ingress/dispatch concern. The presence of
buildable `config.channels[]` entries gates it: with none, `--serve` is unchanged
from the watcher/automation/scheduler shape; with channels, `run_serve` shares the
same `RuntimeAssembly` and provider used by automation (or a scripted offline
`FakeProvider` when no default route exists), registers configured adapters into a
strand-owned `ChannelManager`, logs skipped disabled/unknown kinds, builds
`make_routed_channel_prompt_runner(...)`, and passes registered channel ids into
`serve_body`. The body calls `ChannelManager::start_all()` before racing the
concern. Shutdown is explicit: `serve_channels` marks its pumps stopping, calls
`ChannelManager::stop_all()` to wake adapter-owned receives, and cancels/joins
the pump-owned `async::TaskGroup` before returning.
The in-process `MockChannel::stop()` now closes its bounded inbound queue and
`start()` reopens a fresh one, matching the QQ adapter's transport-close behavior
for pending `next_message()` waits.

Slice 259 hardens that concern's dispatch side. The dispatch loop now consumes manager fan-in
messages and assigns each one to a bounded worker queue keyed by
`(channel_id, conversation_id)`. Each worker runs the same routed
`ChannelPromptRunner` and sends replies sequentially for that one conversation,
while unrelated conversations can await their agent runs concurrently. That
slice used a total worker-completion counter plus a non-blocking progress signal
rather than an awaited done send, so shutdown could close worker queues, emit
child cancellation, and wait for workers without racing their cancellation slot
cleanup.

Slice 260 bounds that worker table for long-lived services. `ServeChannelOptions`
adds a public C++ test/embedding knob for `conversation_queue_capacity` (default
64) and `conversation_idle_ttl` (default 5 minutes); no JSON config field was
added, because the typed `serve` config block is still deferred until more than
one concern needs operator tuning. A worker now races its inbox receive against
the idle TTL. When the TTL wins and the inbox is still empty, the worker exits,
sets a completion flag, and sends a non-blocking progress wake. The dispatcher
erases completed workers on progress wakes and before enqueueing a new message,
so a later message for the same `(channel_id, conversation_id)` gets a fresh
worker instead of landing in an exited inbox. Shutdown waits on per-worker
completion flags rather than a total completed count, so previously evicted
workers cannot make active shutdown waits look complete. Deadlines remain
downstream.

Slice 261 adds the first structured worker metrics boundary. `ServeChannelWorkerMetrics` snapshots report the
current worker table size, max observed worker table size, created/completed
workers, idle evictions, message enqueue count, sent replies, dispatch failures,
and enqueue failures. `ServeChannelOptions::metrics_observer` is an optional
C++ observer called synchronously on the dispatcher executor after worker
creation/erasure, message enqueue, or worker progress wakes. Worker-side reply
and dispatch-failure counters are atomic because workers can run concurrently
when an embedding supplies a non-strand executor; worker-table counters stay
dispatcher-owned. Observer exceptions are caught and reported to stderr so an
embedding/test callback cannot terminate the long-lived service. No JSON config,
daemon metrics endpoint, or hook payload was added in this slice; the observer is
the owner/test seam later consumed by the daemon sink.

Slice 262 adds the first per-message deadline at that same owner/test boundary.
`ServeChannelOptions::message_deadline` is optional and must be positive when
set. Each conversation worker races one routed agent/reply send attempt against
that deadline; when the timer wins, the in-flight attempt is cancelled through
the coroutine cancellation slot and the worker sends a fixed still-working reply
using the same inbound message envelope. `ServeChannelWorkerMetrics` adds
`message_timeouts`; successful fallback sends count as sent replies, while a
failed fallback send counts as a dispatch failure. This slice deliberately does
not add a JSON config field or a durable background rejoin path, because both
need the deferred typed `serve`/channel config and later-reply policy.

Slice 263 binds the worker metrics seam into the `run_serve` daemon path without
adding a new config surface. `ServeChannelMetricsLogSink` formats
`ServeChannelWorkerMetrics` snapshots as one-line `orangutan: channel worker
metrics ...` records, suppresses repeated identical snapshots, and writes to
stderr by default; tests can inject a callback to capture the same formatted
lines. `run_serve` installs this sink whenever at least one configured channel
adapter registers, so channel-worker state is visible in ordinary daemon logs.
There is still no HTTP metrics endpoint or JSON toggle.

## Next Steps

- Route concrete automation notifier output back to CLI/channel/desktop.
- Harden webhook listener shutdown/HTTP semantics once operator feedback names
  the next edge (for example chunked transfer, auth, or a broader HTTP server
  abstraction).
- Slice D (optional): a typed `serve` config block (toggles/intervals) once more than
  one concern wants tuning — the reaping interval is a fixed 1-minute default today
  and the channel deadline is only a C++ owner/test knob.
- Add the durable later-reply/rejoin path for over-deadline channel messages.
- Add a daemon metrics endpoint only once there is a concrete operator consumer.
- Drive the CLI agent loop on a per-agent strand too, and split channel agent
  runs onto per-agent strands where the service-level strand is still too coarse.
- Bind configured hook sinks to the assembly-owned bus once the hook sink models land.
- Add CLI line editor/history on top of the interactive REPL handoff.
