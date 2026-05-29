# Orangutan v2

A C++26, single-binary **LLM ReAct agent runtime**: pluggable model providers, a tool
registry, multi-platform chat channels, tiered memory, hooks & permissions, agent-team
orchestration, an HTTP web UI, and cron-style automation.

It is a ground-up rewrite of the legacy `orangutan/` (C++23, ~40 kLoC) that hit walls on
compile time, a QQ-specific channel monolith, duplicated file/shell tooling, and a
single-threaded web server — full audit in
[`orangutan-legacy-audit.md`](docs/references/orangutan-legacy-audit.md).

> **Agent-first.** Every load-bearing decision lives in a versioned doc under `docs/`, so
> any coding agent can ship without chat memory. Entry point:
> [`CLAUDE.md`](CLAUDE.md) (the `AGENTS.md` symlink) → [`docs/STATUS.md`](docs/STATUS.md).

## Highlights

- **C++26 on GCC 16.1** — modules + PCH, with a per-TU **compile budget** enforced in CI.
- **One async model** — asio standalone + C++20 coroutines, no `stdexec` fork.
- **Channel trait** — QQ, Discord, Slack, Telegram, Webhook and more; each is one library.
- **Provider trait** — Anthropic, OpenAI (Chat & Responses), Gemini, DeepSeek behind a
  single capability matrix.
- **Tiered memory** — working / session / long-term / shared, each with a hookable lifecycle.
- **Hooks everywhere** — tool, agent, provider, memory, channel, and orchestration points.
- **`bench/` beside `tests/`** for every library; A-vs-B comparisons are the default.

## Repository Layout

```
CLAUDE.md            routing index (read first); AGENTS.md is a symlink to it
Makefile             init / check / new-plan / new-history / bench
docs/                system of record — design, rules, specs, status, histories
  STATUS.md          one-screen project snapshot — read first
  ARCHITECTURE.md    library boundaries + binary inventory
  rules/             non-negotiable C++ / build / workflow rules
  design-docs/       deep architectural designs
  product-specs/     user-visible feature specs
scripts/             repo automation (ci, scaffolding, bench-compare)
skeleton/            starting xmake / include / src skeleton
include/  src/       (target) C++ public headers + implementation
tests/    bench/     Catch2 + nanobench buckets, one per library
.github/             CI, PR + issue templates
```

## Quick Start

1. Read [`CLAUDE.md`](CLAUDE.md), then [`docs/STATUS.md`](docs/STATUS.md) for the current state.
2. Install the versioned pre-commit hook once: `git config core.hooksPath .githooks`.
3. Build and smoke-test the binary:

   ```sh
   xmake f -m release && xmake build orangutan
   xmake run orangutan -- --prompt "What is 17 * 23?"
   # provider-backed prompts need a key:
   ANTHROPIC_API_KEY=... xmake run orangutan -- --config config.example.json --prompt "hello"
   ```

4. Before a PR: `make ci`.

## License

Proprietary — same posture as legacy `orangutan/`. Update before publishing.
