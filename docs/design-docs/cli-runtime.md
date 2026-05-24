# CLI Runtime

`oran-cli` owns terminal-facing mode selection and terminal-owned interaction surfaces.
Bootstrap loads config first, then forwards non-bootstrap arguments to CLI. The CLI
accepts prompts and reports the selected mode, but it does not create an agent runtime
or provider request yet. It now also owns the first user-visible
`permission_ask_rendered` sink so direct dispatch can ask a terminal operator through
the blocking hook bus once a caller binds the sink to the process `hook::Bus`.

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

struct OperatorPromptSinkOptions {
  std::string sink_id;
  std::string operator_identity;
  std::vector<std::string> scripted_answers;
  bool quiet;
};

class OperatorPromptSink final : public hook::Sink {
 public:
  explicit OperatorPromptSink(OperatorPromptSinkOptions options = {});

  async::Awaitable<core::Result<hook::HookDecision>>
  handle_blocking(hook::Event event, hook::Payload payload) override;

  std::size_t prompts_rendered() const noexcept;
  std::size_t answers_consumed() const noexcept;
};

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

## Operator Approval Prompt

`cli::OperatorPromptSink` is a concrete `hook::Sink` for
`hook::Event::permission_ask_rendered`. It expects a
`hook::PermissionAskRenderedPayload`, renders the tool name, caller identity, matched
decision reason, replay/TTL policy, request timestamp, and raw input JSON, then asks the
operator for a yes/no answer. Approval answers (`y`, `yes`, `approve`, `approved`,
`proceed`) return `HookDecisionKind::proceed` with
`reason=operator_approved:<identity>`. Denial answers (`n`, `no`, `deny`, `denied`,
`reject`, or an empty answer) return `HookDecisionKind::veto` with
`reason=operator_denied:<identity>`.

`scripted_answers` exists for tests and future noninteractive drivers. When empty, the
sink reads one line from terminal stdin through an asio `posix::stream_descriptor` on the
current coroutine executor. Binding remains explicit: the binary still does not run the
provider-backed agent loop, so bootstrap does not attach this sink to the process bus
until the CLI/binary handoff slice has a dispatch path that can publish asks.

## Bootstrap Handoff

`oran-bootstrap` consumes `--config`, `--config=...`, and global help. Arguments that are
not bootstrap-owned are forwarded unchanged to `cli::run` after config loading and the
default provider-route preflight. This keeps config discovery, provider-route validation,
and terminal mode selection separate while preserving one process entry point.

## Next Steps

- Replace the deterministic REPL shell with a line editor once terminal history/editing is
  needed.
- Route single-shot and REPL prompts into `oran-agent` once provider/tool foundations land.
- Bind `OperatorPromptSink` into the real CLI agent-loop runtime when the binary starts
  driving `agent::Loop`.
- Add slash-command parsing after the runtime has command targets.
