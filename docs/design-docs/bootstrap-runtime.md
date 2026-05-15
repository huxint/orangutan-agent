# Bootstrap Runtime

`oran-bootstrap` is the process entry boundary. It owns command-line bootstrap
parsing, config discovery, and future runtime assembly. The current slice loads
configuration, reports the config source, and hands CLI-facing modes to `oran-cli`.

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
- the `oran-cli` mode output.

No provider credentials are read, no storage files are opened, and no agent runtime loop
is started in this slice.

## Next Steps

- Build runtime assembly around loaded config.
- Initialize storage files and apply domain migrations.
- Add signal handling and cancellation once the agent loop exists.
