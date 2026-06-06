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

struct RuntimeAssemblyOptions {
  std::string audit_db_path{};
  bool audit_enabled = true;
  std::size_t audit_reader_count{1};
  std::size_t audit_statement_cache_capacity{4};
  tool::WorkspaceOptions workspace_options;
  bool trace_enabled = true;
  std::optional<std::int64_t> trace_retention_started_before_ns{};
  std::chrono::milliseconds hook_blocking_timeout{2000};
  std::string sessions_db_path{};
  bool session_memory_enabled = true;
  std::size_t session_reader_count{2};
  std::size_t session_statement_cache_capacity{8};
  std::string longterm_memory_db_path{};
  bool longterm_memory_enabled = true;
  std::size_t longterm_memory_reader_count{2};
  std::size_t longterm_memory_statement_cache_capacity{16};
  std::optional<LongtermMemoryStartupDecayOptions> longterm_memory_startup_decay{};
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
  bool longterm_vector_memory_enabled() const noexcept;
  std::string_view longterm_memory_path() const noexcept;
  std::string_view longterm_vector_memory_path() const noexcept;
};

}  // namespace orangutan::bootstrap
```

The assembly owns the approval broker, audit sink/repository pool, workspace resolver,
optional trace repository, process hook bus, optional session-memory store, and
optional long-term memory backend/runtime.
Session memory uses a separate `storage::Pool` and `storage::SessionRepository` over
`<workspace>/.orangutan/sessions.db` by default; it never shares the audit/trace
`audit.db` pool. Long-term memory uses a separate `storage::Pool` over
`<workspace>/.orangutan/memory.db`, migrates the `memory::longterm::Fts5Backend`
schema before opening the long-lived pool, optionally runs one bounded startup
decay pass from `LongtermMemoryStartupDecayOptions`, and exposes both the backend
and `memory::longterm::Runtime` for prompt-boundary recall and the read-only
`memory.recall` tool plus the write-side `memory.remember` and `memory.forget`
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
by trace retention. For configured-route runs, bootstrap also derives
`longterm_memory_startup_decay` from `memory.longterm.retention` for the runner's
stable `cli` scope. The assembly applies that one lexical-memory pass after
long-term migration and before the long-lived memory pool is exposed, so prompt
and tool reads see the post-decay visible set. It stores the pass result on the
assembly as `longterm_memory_startup_decay_shadowed_count()`: `std::nullopt`
means no startup pass was configured or run, while `0` or higher means the pass
ran and reports how many records were shadowed. `decay_check_interval_hours`
remains a future `oran-automation` cadence input, not a startup-loop timer. The
built-in empty-defaults path disables session and long-term memory so a fresh
checkout can run the deterministic CLI shell without opening `sessions.db` or
`memory.db`. The startup banner prints
`trace=<enabled|disabled>`, `sessions=<enabled|disabled> (<path|disabled>)`,
`longterm-memory=<enabled|disabled> (<path|disabled>)`,
`startup-decay=<disabled|N>`,
`vector-memory=<enabled|disabled> (<path|disabled>)`, and `hook-timeout=<ms>`.

`<oran/bootstrap/prompt_runner.hpp>` exposes the bootstrap-owned CLI runner used by
tests and the ordinary binary handoff for configured routes. `AgentPromptRunner::create`
borrows a caller-supplied `RuntimeAssembly`, typed config, resolved `provider::Route`,
executor, and provider backend; registers the builtin tool catalog; materializes
permission rules from config plus an optional agent overlay; wraps the backend in
`provider::execution::Runtime`; binds `cli::OperatorPromptSink` to the
assembly-owned `permission_ask_rendered` bus; and drives `agent::Loop` with workspace,
audit, broker, hook, output-cap, trace, and session-memory services from the assembly.
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
from the already-loaded conversation transcript, nets successful `skill.invoke`
activation and `skill.deactivate` deactivation tool-results that carry the
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
tool registry. The `memory.recall` built-in parses and gates the call in
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
boundary recall and `memory.recall` use `HybridRuntime`; `memory.remember` and
`memory.forget` mirror vector upserts/deletes when a vector backend is present.
Default builds still reject `enabled=true` before `RuntimeAssembly::build` or
provider construction with `ErrorKind::config`,
`path=$.memory.longterm.hybrid_search.enabled`,
`reason=build_option_disabled`, and `option=vector_memory`.
Slice 169 installs `DispatchContext::memory_remember` beside that recall
binding. The `memory.remember` built-in parses and gates one record-shaped write
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
bindings. The `memory.forget` built-in parses and gates one `{id}` delete in
`oran-tool`; bootstrap adapts it to the assembly-owned
`memory::longterm::Backend`, supplies the runner's stable `scope_key`, calls the
backend's idempotent `remove(...)`, and returns confirmation text, structured
`memory_forget` removed-key metadata, and `usage.bytes_written = 0` through the
same provider re-entry path. Slice 179 publishes advisory `memory_forget` after
that successful scoped delete. Slice 180 publishes advisory `memory_read_after`
after successful prompt-boundary long-term recall and after successful
`memory.recall` tool reads, for both lexical and hybrid recall paths. Memory
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
`skill.invoke` built-in during that runner's current turn: bootstrap installs a
`DispatchContext::skill_invoke` callback, and the tool dispatch path returns the
matched markdown body as ordinary tool-result text for the next provider
iteration without re-reading disk mid-turn. Successful invokes also return a
small structured activation record through `Output::data_json`; the next prompt
boundary feeds that record through `skill::resolve_active_skills(...)` to mark
the skill active in section 4 without parsing model-visible body text.
Slice 147 installs a parallel `DispatchContext::skill_deactivate` callback over
the same filtered document vector: `skill.deactivate` returns a versioned
`skill_deactivation` record in `Output::data_json`, and the next prompt boundary
nets it against prior activations through the same transcript scan so a
mid-session deactivation clears the section-4 marker without a config edit or a
renderer clock. Slice 148 persists successful `skill.invoke` / `skill.deactivate`
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
`tool.search` results is exposed for diagnostics through
`tool_search_observations_recorded()`, and section render counters are exposed
through `system_preamble_renders()` and `memory_framing_renders()`. The runner
does not construct real provider adapters or read provider credentials.

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
The built-in empty-defaults path still reports `provider route: none configured`
and uses the deterministic no-runner `cli::run` shell so fresh checkouts remain
runnable without provider credentials or a sessions DB; selector flags
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

## Next Steps

- Bind configured hook sinks to the assembly-owned bus once the hook sink models land.
- Add CLI line editor/history on top of the interactive REPL handoff.
