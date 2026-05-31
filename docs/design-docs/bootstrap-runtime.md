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
  std::chrono::milliseconds hook_blocking_timeout{2000};
  std::string sessions_db_path{};
  bool session_memory_enabled = true;
  std::size_t session_reader_count{2};
  std::size_t session_statement_cache_capacity{8};
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
};

}  // namespace orangutan::bootstrap
```

The assembly owns the approval broker, audit sink/repository pool, workspace resolver,
optional trace repository, process hook bus, and optional session-memory store.
Session memory uses a separate `storage::Pool` and `storage::SessionRepository` over
`<workspace>/.orangutan/sessions.db` by default; it never shares the audit/trace
`audit.db` pool. `bootstrap::run` threads `config.trace().enabled` into
`trace_enabled`, `config.hooks().timeout_ms` into `hook_blocking_timeout`, and enables
session memory only when a provider route is configured. The built-in empty-defaults
path disables session memory so a fresh checkout can run the deterministic CLI shell
without opening `sessions.db`. The startup banner prints
`trace=<enabled|disabled>`, `sessions=<enabled|disabled> (<path|disabled>)`, and
`hook-timeout=<ms>`.

`<oran/bootstrap/prompt_runner.hpp>` exposes the bootstrap-owned CLI runner used by
tests and the ordinary binary handoff for configured routes. `AgentPromptRunner::create`
borrows a caller-supplied `RuntimeAssembly`, typed config, resolved `provider::Route`,
executor, and provider backend; registers the builtin tool catalog; materializes
permission rules from config plus an optional agent overlay; wraps the backend in
`provider::execution::Runtime`; binds `cli::OperatorPromptSink` to the
assembly-owned `permission_ask_rendered` bus; and drives `agent::Loop` with workspace,
audit, broker, hook, output-cap, trace, and session-memory services from the assembly.
When the assembly exposes `session_store()`, the runner loads the persisted history for
the current `session_id` + `agent_key` before each prompt and appends only the new
successful transcript suffix afterward. The in-process transcript cache remains the
fallback when session memory is disabled. After every successful turn the runner walks
the new transcript suffix and feeds each tool result back through
`agent::SessionState::observe_tool_output(...)` so the next turn's
`promotion_snapshot(...)` sees deferred-tool promotions; the count of observed
`tool.search` results is exposed for diagnostics through
`tool_search_observations_recorded()`. The runner does not construct real provider
adapters or read provider credentials.

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
`cli::run_async`. No-prompt configured-route runs set
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
runnable without provider credentials or a sessions DB. The runtime assembly opens
the audit DB when audit is enabled so migrations, trace repository ownership, and
audit sinks are available before configured-route prompt execution. Configured
provider routes additionally open and migrate the separate sessions DB and expose
the typed `memory::session::Store` for the runner persistence slice.

`orangutan --trace <turn-id>` is a bootstrap-owned one-shot inspector. It
opens the workspace audit DB, runs the idempotent audit migration, loads the
`trace_turns` row, then prints joined `audit_events` rows ordered by `id ASC`.
Since slice 93 those audit lines include `kind=<event_kind>`, so mixed
permission-decision and `hook_publish` rows are readable in the same output.

## Next Steps

- Introduce the once-per-turn memory framing owner after session persistence is
  stable.
- Bind configured hook sinks to the assembly-owned bus once the hook sink models land.
- Add CLI line editor/history on top of the interactive REPL handoff.
