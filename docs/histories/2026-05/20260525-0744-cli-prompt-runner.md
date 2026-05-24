# CLI Prompt Runner Handoff

Slice 99 resolved provider routes at bootstrap startup, but the CLI still had only a
fully deterministic pre-agent shell API. This slice adds the adapter-neutral handoff
seam inside `oran-cli`: callers can keep using `cli::run` for the no-runner shell, or
call `cli::run_async(CliOptions, PromptRunner*)` to delegate parsed single-shot prompts
and non-empty scripted REPL lines to a caller-owned async prompt runner.

The design intentionally keeps provider construction and `agent::Loop` ownership out of
`oran-cli`. Bootstrap can consume this seam in the next handoff slice by constructing a
provider execution runtime, binding the terminal approval sink, and driving the loop
without making the CLI library depend on `oran-agent`.

Release note: `docs/releases/feature-release-notes.md` documents the user-visible
handoff seam. Focused validation: `xmake run test-cli` (14 cases / 97 assertions).
Final validation also ran `xmake run orangutan -- --help`, `make ci`, and
`git diff --check`.

Files of interest:

- `include/oran/cli/cli.hpp`
- `src/oran-cli/cli.cpp`
- `tests/cli/test_cli.cpp`
- `docs/design-docs/cli-runtime.md`
- `docs/STATUS.md`
