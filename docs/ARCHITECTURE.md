# Orangutan v2 — Architecture

This document is the **top-level map** of the rewrite. It defines the target state, not
the current state. Every block here is a place where a future execution plan or product
spec can be slotted in.

> The previous architecture is captured in
> [`../references/orangutan-legacy-audit.md`](references/orangutan-legacy-audit.md).
> Read it before touching subsystems — the friction list there is what this design exists
> to address.

## Mental Model

Orangutan v2 is **a single binary** that hosts **N agent runtimes** behind **M interfaces**
(CLI, web, channels, automation), backed by **shared storage and policy**. Everything is
asynchronous on a single executor; everything that crosses a process boundary goes through
a transport trait; everything observable goes through a hook surface.

```
                           ┌─────────────────────────────────────┐
  CLI REPL ────────────▶   │                                     │
  CLI single-shot ────▶    │       INTERFACE LAYER               │
  Web (HTTP/SSE) ──────▶   │  cli  •  web  •  channel  •  cron   │
  Channels (QQ/…) ─────▶   │                                     │
  Automation (cron) ───▶   └─────────────────┬───────────────────┘
                                             │
                           ┌─────────────────▼───────────────────┐
                           │       AGENT RUNTIME LAYER           │
                           │                                     │
                           │   ReAct loop  ◀───▶  Tool registry  │
                           │      │                  │           │
                           │      ▼                  ▼           │
                           │   Provider system   Permissions     │
                           │      │                  │           │
                           │      ▼                  ▼           │
                           │   Memory tiers      Hook bus        │
                           │      │                  │           │
                           │      └────────┬─────────┘           │
                           │               │                     │
                           │     Orchestration (teams)           │
                           └───────────────┬─────────────────────┘
                                           │
                           ┌───────────────▼─────────────────────┐
                           │       PLATFORM LAYER                │
                           │  storage  •  config  •  secrets     │
                           │  http/ws  •  process  •  io          │
                           │  asio executor  •  logging          │
                           └─────────────────────────────────────┘
```

Each layer has **one direction of dependency**: interfaces depend on agent-runtime,
agent-runtime depends on platform. The platform layer never reaches up.

## Intended Repository Shape

```
harness-engineering/
├── include/oran/<lib>/...   public headers (forward-decl-heavy)
├── src/<lib>/...            implementation (heavy includes confined here)
├── tests/<lib>/...          one Catch2 bucket per library
├── bench/<lib>/...          one nanobench bucket per library
├── skeleton/                copy-paste starter for first xmake build
└── docs/                    system of record
```

The C++ libraries are listed below. **Each library is its own xmake target**, has its
own test bucket, its own bench bucket, and its own public header set under
`include/oran/<lib>/`.

## Library Inventory

> **Slice status (2026-05-16):** `oran-core`, `oran-async`, the file/directory MVP
> of `oran-io`, the expected-only SQLite core + migration runner + async
> writer/reader `Pool` + per-connection `StatementCache` of `oran-storage`,
> the first `oran-config` JSON loader, and the config-loading slice of
> `oran-bootstrap`, plus the first `oran-cli` handoff shell, are implemented.
> All other rows below are *planned* and will land per `docs/exec-plans/` as
> future slices are scheduled. The build system, PCH, tests bucket, and bench
> bucket conventions are live; see the history entries under
> `docs/histories/2026-05/`.

| Library              | Purpose                                         | Depends on (allowed)                          |
| -------------------- | ----------------------------------------------- | --------------------------------------------- |
| `oran-core`          | `Result<T>`, `Error`, `Message`, `Content`, `ToolDef`, base enums, `core::str`, `core::time` | stdlib only |
| `oran-async`         | asio `Runtime`, `Awaitable<T>`, bounded `Channel<T>`, cancel-aware `sleep_for`; mailbox policy lands in orchestration | `oran-core`, asio |
| `oran-log`           | spdlog shim + secret redaction; thread-local context | `oran-core`, spdlog/fmt |
| `oran-io`            | file/directory IO MVP; planned glob, pipe, subprocess, signal | `oran-core`, `oran-async` |
| `oran-http`          | http client (asio) and tiny router for the web UI | `oran-core`, `oran-async` |
| `oran-storage`       | SQLite expected-only connection/statement core, migration runner, async writer/reader `Pool`, and per-connection `StatementCache`; planned domain repositories | `oran-core`, `oran-async`, sqlite3 |
| `oran-config`        | JSON config loader with typed runtime/profile/route/session/web fields and env substitution; planned schema + secret-protected fields | `oran-core`, `oran-storage` |
| `oran-permission`    | runtime allow/deny/ask rule engine | `oran-core` |
| `oran-skill`         | skill loader, skill catalog | `oran-core`, `oran-io` |
| `oran-tool`          | tool registry, tool runtime context, dispatch | `oran-core`, `oran-async`, `oran-permission`, `oran-io` |
| `oran-hook`          | hook bus + sink kinds (shell / lua / in-proc) | `oran-core`, `oran-async`, `oran-io` |
| `oran-memory`        | working / session / long-term / shared memory | `oran-core`, `oran-storage` |
| `oran-provider`      | provider system (transport / protocol / execution) | `oran-core`, `oran-async`, `oran-http` |
| `oran-prompt`        | system prompt assembly with caching | `oran-core`, `oran-memory` |
| `oran-agent`         | the ReAct loop | `oran-core`, `oran-async`, `oran-provider`, `oran-tool`, `oran-memory`, `oran-prompt`, `oran-permission`, `oran-hook` |
| `oran-orchestration` | team + mailbox + coordination strategies | `oran-agent`, `oran-async` |
| `oran-automation`    | cron / periodic / triggered jobs | `oran-agent`, `oran-storage`, `oran-async` |
| `oran-channel`       | `Channel` trait + adapters | `oran-agent`, `oran-async`, `oran-http` |
| `oran-channel-qq`    | QQ adapter (optional, gated) | `oran-channel`, `oran-http` |
| `oran-channel-discord` | Discord adapter (optional, gated) | `oran-channel`, `oran-http` |
| `oran-channel-slack` | Slack adapter (optional, gated) | `oran-channel`, `oran-http` |
| `oran-channel-telegram` | Telegram adapter (optional, gated) | `oran-channel`, `oran-http` |
| `oran-channel-webhook` | generic webhook adapter | `oran-channel`, `oran-http` |
| `oran-web`           | HTTP web UI (cpp-httplib in skeleton, asio later) | `oran-agent`, `oran-orchestration`, `oran-http` |
| `oran-cli`           | early REPL / single-shot shell; planned slash commands and agent handoff | currently `oran-core`; planned `oran-agent`, `oran-orchestration` |
| `oran-bootstrap`     | process entry + config loading + CLI handoff; planned runtime assembly | currently `oran-core`, `oran-config`, `oran-cli`; planned every public lib above |

**Binaries** built on top:

| Binary             | Description                                                  |
| ------------------ | ------------------------------------------------------------ |
| `orangutan`        | Default: CLI REPL or single-shot, optional `--web` and channel modes. |
| `orangutan-server` | Daemon mode: web + channels + automation, no terminal UI.    |
| `orangutan-bench`  | Standalone runner that executes the `bench/<lib>/...` buckets and emits JSON. |

## Boundary Rules

- **One-way dependencies.** Each library lists what it is *allowed* to depend on in the
  table above. CI enforces this with `scripts/check-deps.sh` (to be implemented in the
  build skeleton — see `docs/BUILD_SYSTEM.md`).
- **No globals.** The bootstrap layer owns lifetimes; everything else takes context
  objects by reference. See `docs/design-docs/module-boundaries.md`.
- **No back-channels.** If a feature needs to influence something outside its layer, it
  surfaces a hook or a callback. See `docs/design-docs/permissions-and-hooks.md`.
- **No `friend` across libraries.** Friendship stays in the same library, ideally the
  same TU.
- **Public headers under `include/oran/<lib>/`** are forward-declaration heavy; full
  includes live in `src/<lib>/`. This is enforced by `docs/rules/module-and-pch.md`.

## Data Flow (Single Turn)

```
1.  Inbound message arrives via cli / web / channel / automation.
2.  oran-bootstrap routes it to the right Agent (per agent_key + identity).
3.  oran-agent::run(prompt):
      a. hook bus  → AgentLifecycle::iteration_start
      b. oran-prompt → build cached prompt (memory section computed ONCE)
      c. oran-provider → send (transport → protocol → execution)
      d. parse response → for each tool_use:
           - oran-permission → check
           - hook bus → ToolLifecycle::before
           - oran-tool → dispatch
           - hook bus → ToolLifecycle::after
      e. append history → loop until stop_reason = end_turn or MAX_ITERATIONS
      f. hook bus → AgentLifecycle::iteration_end / final_response
4.  oran-storage persists conversation.
5.  oran-memory distills new long-term facts (async, off the critical path).
```

## Async Topology

- **One executor** (`asio::io_context` wrapped in `oran::async::Runtime`) drives all I/O.
- **Worker pool** is asio's thread pool; size comes from `config.runtime.workers`.
  The current config default is `4`; hardware-aware bootstrap defaulting can replace
  that when `oran-bootstrap` owns runtime assembly.
- Long-running CPU work (memory distillation, prompt assembly when very large) runs on
  `oran::async::cpu_pool`, a separate fixed-size pool, surfaced as `asio::any_io_executor`.
- See `docs/design-docs/async-model.md` for the full topology.

## State Layer

- **SQLite** for sessions, memory, automation jobs, hook audit logs.
- **One database file per concern**: `sessions.db`, `memory.db`, `automation.db`,
  `audit.db`. Crash isolation; smaller WAL contention.
- **WAL mode** by default; one writer connection per DB, read-pool for queries.
- **Migrations** versioned and applied at startup; see
  `docs/design-docs/secrets-and-state.md`.

## Configuration

- `config.example.json` is checked in and load-tested by `oran-config`.
- `oran-bootstrap` now accepts `--config <path>` / `--config=<path>`. Explicit paths
  are required to load successfully; without `--config`, bootstrap loads
  `<workspace>/.orangutan/config.json` when present and uses built-in config defaults
  when it is absent in this early runtime slice. After config loading, bootstrap hands
  CLI mode flags such as `--prompt` to `oran-cli`.
- The current typed surface covers `strict_config`, `runtime`, `profiles`, `routes`,
  `session`, and `web`; planned sections such as channels, teams, hooks, memory, and
  automation are accepted as recognized root fields until their typed models land.
- `${VAR}` and `${VAR:-default}` substitutions run on string values at load time.
- Secret encryption, generated JSON Schema, and rotation remain planned follow-up
  slices. See `docs/design-docs/secrets-and-state.md`.

## To Fill In As The Project Matures

This file should grow with the project but stay scannable. The expected next edits are:

- Datapath diagrams per channel adapter once the first three adapters land.
- Identity / scope diagram once memory tiers ship.
- Observability stack diagram (logs / metrics / traces) once shipping.
- Deployment topology once a real runtime target exists.
