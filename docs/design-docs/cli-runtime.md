# CLI Runtime

`oran-cli` owns terminal-facing mode selection and terminal-owned interaction surfaces.
Bootstrap loads config first, then forwards non-bootstrap arguments to CLI. The CLI can
parse prompts through the deterministic pre-agent shell or delegate them to a caller-owned
async `PromptRunner`; it still does not construct providers or an `agent::Loop` itself.
It also owns the first user-visible `permission_ask_rendered` sink so direct dispatch can
ask a terminal operator through the blocking hook bus once a caller binds the sink to the
process `hook::Bus`.

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

struct PromptRunRequest {
  std::string prompt;
  CliMode mode;
  std::size_t prompt_index;
};

struct PromptRunResult {
  std::string text;
};

class PromptRunner {
 public:
  virtual ~PromptRunner() = default;

  [[nodiscard]] virtual async::Awaitable<core::Result<PromptRunResult>>
  run_prompt(PromptRunRequest request) = 0;
};

async::Awaitable<core::Result<CliResult>>
run_async(CliOptions, PromptRunner* runner = nullptr);

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

Public functions return `core::Result<T>` directly or inside the project coroutine
vocabulary, `async::Awaitable<core::Result<T>>`. `quiet` exists so tests and benches can
exercise the dispatch path without printing per-iteration output.

## Mode Selection

`cli::run` accepts:

- `--prompt <text>`
- `--prompt=<text>`
- `--help` / `-h`

No prompt flag selects `CliMode::repl`. The current REPL shell is deterministic: it
reports that the shell is ready and, when `repl_lines` are supplied by tests or future
callers, counts non-empty lines as prompts. `run_async` calls the supplied
`PromptRunner` once for each non-empty scripted REPL line in order, with
`prompt_index` counting only dispatched prompts. Without a runner, the shell preserves
the existing pre-agent-loop output and does not read provider credentials, open storage,
or run tools.

Single-shot mode accepts exactly one prompt and returns `prompts_processed = 1`.
`run_async` passes that prompt to the supplied runner with
`CliMode::single_shot` / `prompt_index = 0`; runner errors propagate unchanged.
Duplicate or empty prompt values are `invalid_argument` errors.

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
current coroutine executor. Binding remains explicit: `AgentPromptRunner` binds this
sink to the assembly-owned process bus when a caller supplies a provider backend, while
the ordinary binary path still stays on the deterministic no-runner shell until real
provider adapter construction exists.

## Bootstrap Handoff

`oran-bootstrap` consumes `--config`, `--config=...`, and global help. Arguments that are
not bootstrap-owned are forwarded unchanged to `cli::run` after config loading and the
default provider-route preflight. This keeps config discovery, provider-route validation,
and terminal mode selection separate while preserving one process entry point. The
binary still uses the no-runner path, but bootstrap now exports
`AgentPromptRunner`, a caller-owned `PromptRunner` implementation that wraps a supplied
provider backend in `provider::execution::Runtime`, binds `OperatorPromptSink`, and
drives `agent::Loop` with runtime-assembly services. Tests exercise that path through
`cli::run_async`; ordinary `bootstrap::run` will switch only after real adapters can be
constructed from config.

## Next Steps

- Replace the deterministic REPL shell with a line editor once terminal history/editing is
  needed.
- Construct real provider adapter backends from config and hand them to bootstrap's
  `AgentPromptRunner` from the ordinary binary path.
- Add slash-command parsing after the runtime has command targets.
