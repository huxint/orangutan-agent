# Bootstrap Agent Prompt Runner

Slice 100 gave `oran-cli` an adapter-neutral prompt-runner seam, but bootstrap still
had no owner that could supply runtime-assembly services to `agent::Loop`. This slice
adds `bootstrap::AgentPromptRunner`: a `cli::PromptRunner` implementation that borrows
the process assembly, typed config, executor, resolved provider route, and a
caller-supplied `provider::System`.

The design keeps real adapter construction out of the runner. Tests and future adapter
factories can pass any backend; the runner wraps it in `provider::execution::Runtime`,
registers the builtin tool catalog, materializes permission rules, binds
`cli::OperatorPromptSink`, builds a `tool::DispatchContext` from the assembly's
workspace/audit/broker/hook/output-cap services, carries the assembly trace repository
into `RunTurnInputs::trace`, and retains transcript state across prompts. Regular
`bootstrap::run` deliberately remains on the deterministic no-runner CLI path until
Anthropic/OpenAI provider systems can be constructed from config.

Release note: `docs/releases/feature-release-notes.md` documents the bootstrap runner
handoff. Focused validation: `xmake build test-bootstrap` and
`xmake run test-bootstrap` (63 cases / 259 assertions).
Final validation also ran `xmake build orangutan`, `xmake run orangutan -- --help`,
`make ci`, and `git diff --check`.

Files of interest:

- `include/oran/bootstrap/prompt_runner.hpp`
- `src/oran-bootstrap/prompt_runner.cpp`
- `tests/bootstrap/test_prompt_runner.cpp`
- `xmake/targets.lua`
- `docs/design-docs/bootstrap-runtime.md`
- `docs/design-docs/cli-runtime.md`
- `docs/STATUS.md`
