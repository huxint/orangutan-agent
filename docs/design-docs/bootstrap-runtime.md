# Bootstrap Runtime

`oran-bootstrap` is the process entry boundary. It owns command-line bootstrap
parsing, config discovery, and runtime assembly for the services the early binary can
construct before the provider-backed agent loop is wired.

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
CLI args to `cli::run`.

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
};

}  // namespace orangutan::bootstrap
```

The assembly owns the approval broker, audit sink/repository pool, workspace resolver,
optional trace repository, and the process hook bus. `bootstrap::run` threads
`config.trace().enabled` into `trace_enabled` and `config.hooks().timeout_ms` into
`hook_blocking_timeout`; the startup banner prints both `trace=<enabled|disabled>` and
`hook-timeout=<ms>`.

`<oran/bootstrap/prompt_runner.hpp>` exposes the bootstrap-owned CLI runner used by
tests and the future binary handoff. `AgentPromptRunner::create` borrows a
caller-supplied `RuntimeAssembly`, typed config, resolved `provider::Route`, executor,
and provider backend; registers the builtin tool catalog; materializes permission rules
from config plus an optional agent overlay; wraps the backend in
`provider::execution::Runtime`; binds `cli::OperatorPromptSink` to the
assembly-owned `permission_ask_rendered` bus; and drives `agent::Loop` with workspace,
audit, broker, hook, output-cap, and trace services from the assembly. The runner does
not construct real provider adapters or read provider credentials.

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
  blocking hook timeout,
- the `oran-cli` mode output.

When config declares routes, bootstrap resolves the `default` route through
`provider::resolve_route_profiles` and then builds
`provider::make_adapter_construction_plan` before CLI handoff. That startup
preflight catches bad profile references, provider labels, explicit
profile-protocol spellings, missing endpoint metadata, and unsupported endpoint
schemes before the future loop boundary while preserving the non-secret startup
summary. `api_key_env` remains a string field only here: even though
`oran-provider` now exposes `provider::resolve_adapter_credentials(plan)` and
`provider::make_adapter_system(credentials, factories)` plus the slice-109
`provider::ProtocolTransportAdapterFactory` for future adapter factories, and
slice 110 adds the platform `oran-http::Client` body transport. Regular
bootstrap does not call any of those boundaries yet. No provider credentials
are read, no concrete transport is allocated, no provider adapter is
constructed, and no agent runtime loop is started in this slice.
The runtime assembly opens the audit DB when audit is enabled so migrations, trace
repository ownership, and audit sinks are ready before the future loop handoff.
The `AgentPromptRunner` public seam can run `agent::Loop` when a caller supplies a
provider backend (tests use `provider::FakeProvider`), but regular `bootstrap::run`
still calls the deterministic no-runner `cli::run` path until real provider adapter
construction exists.

`orangutan --trace <turn-id>` is a bootstrap-owned one-shot inspector. It
opens the workspace audit DB, runs the idempotent audit migration, loads the
`trace_turns` row, then prints joined `audit_events` rows ordered by `id ASC`.
Since slice 93 those audit lines include `kind=<event_kind>`, so mixed
permission-decision and `hook_publish` rows are readable in the same output.

## Next Steps

- Bind configured hook sinks to the assembly-owned bus once the hook sink models land.
- Construct real provider adapter backends from config by adapting
  `http::Client` to `provider::ProtocolTransport`, and switch the ordinary
  binary prompt path from `cli::run` to `cli::run_async` with
  `AgentPromptRunner`.
