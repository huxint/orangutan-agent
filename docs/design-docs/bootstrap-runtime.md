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
- runtime assembly summary including audit path, workspace root, trace state, and
  blocking hook timeout,
- the `oran-cli` mode output.

No provider credentials are read and no agent runtime loop is started in this slice.
The runtime assembly opens the audit DB when audit is enabled so migrations, trace
repository ownership, and audit sinks are ready before the future loop handoff.

`orangutan --trace <turn-id>` is a bootstrap-owned one-shot inspector. It
opens the workspace audit DB, runs the idempotent audit migration, loads the
`trace_turns` row, then prints joined `audit_events` rows ordered by `id ASC`.
Since slice 93 those audit lines include `kind=<event_kind>`, so mixed
permission-decision and `hook_publish` rows are readable in the same output.

## Next Steps

- Bind configured hook sinks to the assembly-owned bus once the hook sink models land.
- Thread the assembly services into the provider-backed agent loop and CLI handoff.
