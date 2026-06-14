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
  bool interactive_repl = false;
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
  handle_blocking(hook::Event event, hook::PayloadPtr payload) override;

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

No prompt flag selects `CliMode::repl`. `repl_lines` are the scripted REPL path for
tests and noninteractive drivers; both `run` and `run_async` ignore empty scripted
entries. REPL lines that exactly match a recognized slash command after trimming are
handled by `oran-cli` before prompt dispatch: `/help` renders the command list, and
`/exit` / `/quit` stop the REPL. Leading and trailing ASCII whitespace is ignored for
command matching. `run_async` calls the supplied `PromptRunner` once for each
non-command prompt in order with `prompt_index` counting only dispatched prompts.

`interactive_repl` enables terminal input only for the async runner path. When
`run_async` receives a non-null `PromptRunner`, no scripted `repl_lines`, and
`interactive_repl=true`, it reads terminal stdin on the current coroutine executor until
an empty line, `/exit`, `/quit`, or EOF. Each non-command line is dispatched to the
runner as `CliMode::repl`; the prompt is not echoed because the operator already typed
it at the terminal prompt. `/help` is handled locally and does not increment
`prompts_processed`. Scripted lines always win over interactive input, including an
all-empty scripted span, so tests and noninteractive callers never accidentally block on
stdin. Without a runner, the shell preserves the no-runner output and does not read
provider credentials, open storage, block on stdin, or run tools.

Single-shot mode accepts exactly one prompt and returns `prompts_processed = 1`.
`run_async` passes that prompt to the supplied runner with
`CliMode::single_shot` / `prompt_index = 0`; runner errors propagate unchanged.
Duplicate or empty prompt values are `invalid_argument` errors.

## Operator Approval Prompt

`cli::OperatorPromptSink` is a concrete `hook::Sink` for
`hook::Event::permission_ask_rendered`. It expects a
`hook::PermissionAskRenderedPayload`, renders the tool name, caller identity, matched
decision reason, replay/TTL policy, request timestamp, and the bus-delivered input JSON,
then asks the operator for a yes/no answer. For sensitive mutation tools such as
`FileWrite` / `FileEdit`, the prompt sink uses the default hook sink kind and therefore
receives the redacted summary unless a future caller installs an explicitly trusted-local
approval sink. Approval answers (`y`, `yes`, `approve`, `approved`, `proceed`) return
`HookDecisionKind::proceed` with
`reason=operator_approved:<identity>`. Denial answers (`n`, `no`, `deny`, `denied`,
`reject`, or an empty answer) return `HookDecisionKind::veto` with
`reason=operator_denied:<identity>`.

`scripted_answers` exists for tests and future noninteractive drivers. When empty, the
sink reads one line from terminal stdin through an asio `posix::stream_descriptor` on the
current coroutine executor. Binding remains explicit: `AgentPromptRunner` binds this
sink to the assembly-owned process bus when a caller supplies a provider backend, and
configured-route `bootstrap::run` now supplies the HTTP-backed provider runner from the
ordinary binary path.

## Bootstrap Handoff

`oran-bootstrap` consumes `--config`, `--config=...`, and global help. Arguments that are
not bootstrap-owned are forwarded unchanged to `cli::run` only when no provider route is
configured. When config declares a `default` route, bootstrap constructs the
HTTP-backed provider backend, creates `AgentPromptRunner`, and calls
`cli::run_async` with `interactive_repl=true`. This keeps config discovery,
provider-route validation, provider construction, and terminal mode selection separate
while preserving one process entry point. `AgentPromptRunner` wraps the supplied provider backend in
`provider::execution::Runtime`, binds `OperatorPromptSink`, and drives `agent::Loop`
with runtime-assembly services.

## Next Steps

- Add a line editor/history once terminal editing is needed.
- Add more slash-command targets only when bootstrap or runtime surfaces expose
  concrete operations for them.
