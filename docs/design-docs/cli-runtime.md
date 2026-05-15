# CLI Runtime

`oran-cli` owns terminal-facing mode selection. It is intentionally small in this
slice: bootstrap loads config first, then forwards non-bootstrap arguments to CLI.
The CLI accepts prompts and reports the selected mode, but it does not create an agent
runtime or provider request yet.

## Current Public API

```cpp
namespace orangutan::cli {

enum class CliMode {
  help,
  repl,
  single_shot,
};

struct CliOptions {
  std::span<const std::string_view> args;
  std::span<const std::string_view> repl_lines;
  bool quiet = false;
};

struct CliResult {
  CliMode mode;
  std::size_t prompts_processed;
  int exit_code;
};

core::Result<CliResult> run(CliOptions);

}  // namespace orangutan::cli
```

All public functions return `core::Result<T>`. `quiet` exists so tests and benches can
exercise the dispatch path without printing per-iteration output.

## Mode Selection

`cli::run` accepts:

- `--prompt <text>`
- `--prompt=<text>`
- `--help` / `-h`

No prompt flag selects `CliMode::repl`. The current REPL shell is deterministic: it
reports that the shell is ready and, when `repl_lines` are supplied by tests or future
callers, counts non-empty lines as prompts. It does not read provider credentials, open
storage, or run tools.

Single-shot mode accepts exactly one prompt and returns `prompts_processed = 1`. Duplicate
or empty prompt values are `invalid_argument` errors.

## Bootstrap Handoff

`oran-bootstrap` consumes `--config`, `--config=...`, and global help. Arguments that are
not bootstrap-owned are forwarded unchanged to `cli::run` after config loading. This keeps
config discovery and terminal mode selection separate while preserving one process entry
point.

## Next Steps

- Replace the deterministic REPL shell with a line editor once terminal history/editing is
  needed.
- Route single-shot and REPL prompts into `oran-agent` once provider/tool foundations land.
- Add slash-command parsing after the runtime has command targets.
