# 0001 — Core ReAct Loop

## User Problem

Operators want a single binary that runs an LLM in a tool-using ReAct loop, locally,
with persistent session history and a CLI surface. The first deliverable is "I can run
`orangutan`, paste a prompt, and the agent reasons + calls tools + answers".

## Scope (v1)

- Current pre-loop bootstrap slice:
  - `--config <path>` / `--config=<path>` loads a config through `oran-bootstrap`.
  - No `--config` loads `<workspace>/.orangutan/config.json` when present, otherwise
    uses built-in config defaults.
  - `--prompt <text>` / `--prompt=<text>` is handed to `oran-cli` single-shot mode.
  - No CLI mode args select a minimal REPL shell.
  - `--help` / `-h` prints the current bootstrap usage.
  - The current CLI shell is deterministic and pre-agent-loop; it accepts prompts but
    does not call a provider yet.
  - When config declares routes, bootstrap preflights the `default` provider route
    through `provider::resolve_route_profiles` and
    `provider::make_adapter_construction_plan`, reports the resolved
    primary/fallback summary, preserves the profile endpoint metadata needed by the
    future adapter factory, and fails fast on bad profile references, provider
    spellings, explicit profile protocol spellings, missing endpoint metadata, or
    unsupported endpoint schemes. Built-in empty defaults still run without a
    provider route.
  - `oran-provider` exposes `resolve_adapter_credentials(plan)` and
    `make_adapter_system(credentials, factories)` for future real adapter
    construction. The ordinary binary does not call either boundary yet, so
    configured `api_key_env` names are still not read during current startup.
  - Bootstrap exports `AgentPromptRunner` for tests and future adapter owners: callers
    can supply a provider backend and resolved route to drive `agent::Loop` with the
    runtime assembly's workspace/audit/broker/hook/trace services. The ordinary binary
    still stays on the deterministic no-runner shell until real provider adapters can
    be constructed from config.
- One agent runtime per process (multiplexing comes in spec 0004).
- Anthropic Messages **and** OpenAI Chat Completions providers (one of the two
  configured + working end-to-end is acceptance; the other is built and bench-only).
- CLI REPL **and** single-shot (`--prompt`) modes.
- Tool calls dispatched through `oran-tool` to:
  - `file.read`
  - `file.write`
  - `file.edit`
  - `file.search`
  - `shell.exec` (sandboxed)
- Session history persisted to `sessions.db`.
- Hook bus skeleton — at least `tool.before` and `tool.after` events firing to a shell
  sink.
- Permission engine — `default` mode, with a config-driven allow/deny/ask list.
- SIGINT cancels the in-flight iteration cleanly.

## Out Of Scope (v1)

- Web UI (spec 0007).
- Channels (spec 0003).
- Orchestration (spec 0004).
- Automation (spec 0006).
- Long-term memory tier (spec 0005 v1.1).
- Skills loading (spec 0009).

## Acceptance Criteria

1. `xmake build orangutan` produces a binary within the compile-budget envelope.
2. `./orangutan --prompt "Read this README and summarize it in one paragraph"` returns
   a sensible answer in single-shot mode.
3. The REPL renders streaming tokens character-by-character.
4. `Ctrl-C` during a tool call cancels within 1 s.
5. After 100 turns of a conversation, the session file is < 1 MB and re-loadable.
6. The permission engine refuses `shell.exec("rm -rf ...")` by default with an
   audited entry.
7. A `tool.after` shell hook fires for every tool call, observable in
   `<workspace>/.orangutan/audit.db`.
8. `xmake run test-agent` passes with at least 80% coverage of `oran-agent`.
9. `bench/agent/` reports the per-iteration overhead at ≤ 5 ms (excluding provider
   round-trip).

## Design Doc Cross-References

- [`../design-docs/agent-platform.md`](../design-docs/agent-platform.md)
- [`../design-docs/async-model.md`](../design-docs/async-model.md)
- [`../design-docs/api-portability.md`](../design-docs/api-portability.md)
- [`../design-docs/tool-runtime.md`](../design-docs/tool-runtime.md)
- [`../design-docs/permissions-and-hooks.md`](../design-docs/permissions-and-hooks.md)

## Execution Plan

To be created via `make new-plan SLUG=mvp-react-loop` once code starts.

## User Experience Sketch

```sh
$ orangutan
Welcome to Orangutan v2. Type your message; Ctrl-C cancels; /help for commands.

> Find the largest C++ file in this repo and explain its top-level structure.

[thinking...]
[tool: file.search { pattern = "*.cpp", sort = "size" }]
  → src/oran-agent/loop.cpp (4128 lines)
[tool: file.read { path = "src/oran-agent/loop.cpp", lines = "1-80" }]

The largest file is src/oran-agent/loop.cpp at 4128 lines. Its top-level structure:

1. Header includes and using-declarations (lines 1–24).
2. Anonymous-namespace helpers for prompt assembly (25–95).
...
```

## Risks

- Streaming-token rendering on Windows terminals — defer Windows polish.
- Provider rate limits during MVP testing — use fakes by default in CI.
- Cancellation propagation through libcurl mid-stream — verify with `bench/agent/
  cancel-latency` scenario.

## Open Questions

- Default model: Anthropic Sonnet vs. Claude Opus? → resolved by config; example uses
  Sonnet 4 series.
- Default permission mode in MVP: `default` or `strict`? → `default`; users can opt
  into `strict` via config.

## Validation

```sh
xmake build orangutan
xmake run orangutan
xmake run orangutan -- --config config.example.json
xmake run orangutan -- --config config.example.json --prompt "hello"
xmake test test-agent
xmake run orangutan -- --prompt "What is 17 * 23?"
xmake run orangutan -- --prompt "Read README.md and summarize."
scripts/bench-compare.sh agent
```
