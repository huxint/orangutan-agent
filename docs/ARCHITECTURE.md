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

> **Slice status (2026-05-22):** `oran-core` (now with `Error`/`Result`, the
> `Time` value type and ISO-8601 UTC helpers, the conversation types
> `Role`, `StopReason`, `Content` variant, and `Message`, the
> `ToolDef` declaration type (with `required_capabilities`), the
> `core::str` RFC-3629 UTF-8
> helpers, and the `Capability` vocabulary that ties tools to
> permission rules), `oran-async`,
> the file/directory MVP of `oran-io`, the expected-only SQLite core +
> migration runner + SQL-file migration loader + async writer/reader `Pool`
> with per-slot statement caches + standalone per-connection `StatementCache`
> + `SessionRepository` (typed `core::Role` boundary) of `oran-storage`,
> the `oran-config` JSON loader with `runtime`/`profiles`/`routes`/
> `session`/`web` plus the new `permissions` and
> `agents.<name>.permissions` typed surfaces (layer-2/3 data of the
> three-layer rule merge),
> the foundation `RuleSet` + `Decision` of `oran-permission`
> with capability-aware gating (`Rule::capability`,
> `core::Capability`), the `Defaults::for_mode` baseline factory,
> and the three-layer `materialize` merge that concatenates
> defaults + global config + per-agent overlay,
> the config-loading + `RuntimeAssembly` slice of `oran-bootstrap`
> (a value-type bundle holding a fresh `permission::ApprovalBroker`
> and the active `permission::AuditSink` — `StorageAuditSink`
> over an internal `Pool` + `AuditRepository` when audit is on,
> `NullAuditSink` otherwise; audit defaults to enabled now that
> `oran-storage` ships its migrations compile-time-embedded via
> `#embed`; slice 16 adds `--mode` / `--agent` selectors to
> `--explain-rules` via the public `parse_explain_rules_selector`
> and `materialize_rules` helpers), the first `oran-cli` handoff
> shell, the slice-17/18/19/20/21 `oran-tool` foundation (`tool::Registry`
> with `add` / `remove` / `find` / `catalog` / `dispatch`;
> `Registry::add` rejects malformed tool declarations, including
> invalid `input_schema_json` (parseable JSON Schema object plus
> common keyword-type checks, with heavy JSON isolated in
> `src/oran-tool/schema_validation.cpp`); `entries_` uses
> transparent string hashing so lookup by `std::string_view` in
> `remove`, `find`, and `dispatch` does not allocate a temporary key;
> `tool::Workspace` (slice 37) canonicalises tool-layer paths without
> exposing `<filesystem>` in the public header, and
> `DispatchContext::workspace` is the interim non-owning seam file
> built-ins consume until `tool::Runtime::workspace()` lands; slice 38
> adds `file.write` / `file.edit` / `file.delete` adoption through that
> seam, leaving `file.search` and `directory.list` as the remaining
> raw-path built-ins; the
> dispatch path runs `RuleSet::evaluate` against the call's
> `ToolDef::required_capabilities`, records one
> `permission::AuditEvent` per call with
> `input_hash = SHA-256(input_json)` onto the supplied `AuditSink`,
> then branches `allow` → handler, `deny` → `permission_denied`,
> `ask` → consults the optional `(ApprovalBroker*, ApprovalToken*)`
> pair carried on `DispatchContext` and remaps the audit outcome
> to `approved` (runs handler) or `rejected` (forwards the
> broker's `reason` context entry verbatim); when no broker or
> no token is supplied the short-circuit `permission_denied` /
> `reason=approval_required` path is preserved but the error
> now also carries `decision_reason` + `replay_max` +
> `approval_ttl_seconds` so the agent loop can hand them
> straight to `ApprovalBroker::approve`; built-ins `file.read`
> (slice 17, `tool::register_file_read`; slice 37 resolves the input
> through `tool::Workspace::resolve_read` when `DispatchContext::workspace`
> is supplied), `file.write` (slice 18,
> `tool::register_file_write`, capability `write_file`, input
> `{path, content, mode?, create_parents?, max_bytes?}` with
> `mode ∈ {truncate (default), append, fail_if_exists}` and a
> 16 MiB default / hard ceiling on `max_bytes`; slice 38 resolves
> through `tool::Workspace::resolve_write` when supplied), `file.edit`
> (slice 19, `tool::register_file_edit`, capability `edit_file`,
> input `{path, old_string, new_string, replace_all?, max_bytes?}` —
> `conflict` if `old_string` is not unique unless `replace_all` is
> set, `not_found` if `old_string` is absent, and the read + final
> replacement output are capped by `max_bytes`; slice 38 resolves
> through `tool::Workspace::resolve_write` when supplied), `file.search`
> (slice 20, `tool::register_file_search`, capability `read_file`,
> input `{path, pattern, max_matches?, include_hidden?, regex?}` —
> literal substring by default, or re2 partial-match when
> `regex=true` (slice 24, via `permission::InputPattern`);
> single-file or recursive directory walk via
> `std::filesystem::recursive_directory_iterator`; binary
> heuristic skips NUL-bearing files during walks; dotfile-skip by
> default; ripgrep-class optimisations deferred to follow-up
> slices tracked in `exec-plans/tech-debt-tracker.md`),
> `directory.list` (slice 29, `tool::register_directory_list`,
> capability `list_directory` (new in `core::Capability`), input
> `{path, include_hidden?, max_entries?}` — single-level
> enumeration through `oran-io::list_directory`; renders one
> `<path>:<kind>:<size_bytes or '-'>` line per entry sorted by
> path, the literal text `no entries` when the directory is empty
> after the hidden filter, and propagates the `io: directory
> entry limit exceeded` error verbatim when `max_entries` is
> exceeded — raise the cap and retry), and `file.delete`
> (slice 30, `tool::register_file_delete`, capability
> `delete_path` (first built-in that exercises this slice-7
> capability), input `{path}` — refuses non-regular files
> (directories and symlinks reject as `invalid_argument` from
> `oran-io::delete_file`), `not_found` when the file does not
> exist, and returns the literal success message
> `deleted <path>`; slice 38 resolves through
> `tool::Workspace::resolve_delete` when supplied; future direction is a unified delete tool
> covering both files and folders, not per-kind splits)),
> and the slice-22 `oran-hook` foundation (`hook::Event` enum
> covering the 41 lifecycle events the design contemplates,
> `hook::Mode { advisory, blocking }` + `default_mode(Event)`
> annotation, abstract `hook::Sink`, `hook::InProcessSink`
> `std::function`-backed implementation, and `hook::Bus` with
> bind / unbind / `publish_advisory` — the bus iterates sinks
> in subscription order, captures per-sink `Result<void>` in
> `PublishOutcome`, and never aborts on a sink error; blocking
> semantics with veto remain a follow-up slice. `Registry::dispatch`
> grew an optional `DispatchContext::bus` (`hook::Bus*`) that
> publishes `hook::Event::tool_before` after the registry
> resolves the tool def, `hook::Event::tool_dispatched` between
> audit success and the handler co_await on the paths where the
> handler will actually run (slice 25 — `allow` or ask-approved,
> carrying the rule verdict wire spelling),
> `hook::Event::tool_error` on every error exit (slice 25 — handler
> failure, permission deny, broker rejection, audit error, ask
> short-circuit; failure-only narrow channel), and
> `hook::Event::tool_after` at every exit path — all four advisory,
> sinks observe but cannot change the dispatch result),
> are implemented.
> All other rows below are *planned* and will land per `docs/exec-plans/` as
> future slices are scheduled. The build system, PCH, tests bucket, and bench
> bucket conventions are live; see the history entries under
> `docs/histories/2026-05/`.

| Library              | Purpose                                         | Depends on (allowed)                          |
| -------------------- | ----------------------------------------------- | --------------------------------------------- |
| `oran-core`          | `Result<T>`, `Error`, `Time` + ISO-8601 UTC helpers, `Role`, `StopReason`, `Content` variant, `Message`, `ToolDef` (with `required_capabilities`), `core::str` UTF-8 helpers, `Capability` vocabulary (20 enumerators, slice 29 adds `list_directory`) | stdlib only |
| `oran-async`         | asio `Runtime`, `Awaitable<T>`, bounded `Channel<T>`, cancel-aware `sleep_for`; mailbox policy lands in orchestration | `oran-core`, asio |
| `oran-log`           | spdlog shim + secret redaction; thread-local context | `oran-core`, spdlog/fmt |
| `oran-io`            | file/directory IO MVP — `read_text_file`, `write_text_file`, `list_directory`, and `delete_file` (slice 30, regular-file only); planned glob, pipe, subprocess, signal | `oran-core`, `oran-async` |
| `oran-http`          | http client (asio) and tiny router for the web UI | `oran-core`, `oran-async` |
| `oran-storage`       | SQLite expected-only connection/statement core, migration runner with SQL-file loading **and compile-time-embedded built-in migrations** (`built_in_audit_migrations()` / `built_in_session_migrations()` reach the SQL via C++26 `#embed`), async writer/reader `Pool` with per-slot `StatementCache`, standalone per-connection `StatementCache`, `SessionRepository` (typed `core::Role` at the API boundary), and `AuditRepository` (typed audit-event append/list/count over `audit_events`); planned memory/automation repositories | `oran-core`, `oran-async`, sqlite3 |
| `oran-config`        | JSON config loader with typed runtime/profile/route/session/web fields, env substitution, and the typed `permissions` + `agents.<name>.permissions` overlay surface (layer-2/3 data of the three-layer rule merge); planned schema + secret-protected fields | `oran-core`, `oran-storage` |
| `oran-permission`    | foundation rule evaluator: `Verdict`, `Mode`, `Rule`, `RuleSet`, `Decision`, `*`-glob tool matching, capability-aware gating (`Rule::capability` of `core::Capability`), the `Defaults::for_mode` baseline factory, the three-layer `materialize(Mode, global, per_agent)` merge that concatenates defaults + global config + per-agent overlay, the `ApprovalSecret` / `ApprovalAuthority` / `ApprovalToken` / `ApprovalBroker` ask-flow surface, and the `AuditEvent` / `AuditSink` / `StorageAuditSink` audit pipeline; planned re2 input regex extensions and bootstrap wiring | `oran-core`, `oran-config`, `oran-storage`, `oran-async` |
| `oran-skill`         | skill loader, skill catalog | `oran-core`, `oran-io` |
| `oran-tool`          | tool registry (`Registry::add` validates declarations and `input_schema_json` before insertion; `Registry::entries_` uses transparent string lookup for `std::string_view` names), `tool::Workspace` path resolver (`resolve_read` / `resolve_write` / `resolve_delete` / `resolve_list`, with canonical roots, traversal rejection, symlink policy, and extra read/write roots) plus the interim non-owning `DispatchContext::workspace` seam, dispatch through `permission::RuleSet` + `permission::AuditSink` with the slice-21 `(ApprovalBroker*, ApprovalToken*)` mediation of `Verdict::ask` (audit outcome promoted to `approved` on broker.check OK, `rejected` on broker rejection — `expired` / `tool_mismatch` / `identity_mismatch` / `input_mismatch` / `mac_mismatch` / `no_grant` / `replay_exhausted` reason forwarded; the short-circuit `approval_required` path is preserved when no broker/token is supplied but now carries `decision_reason` / `replay_max` / `approval_ttl_seconds` in the error context) and the slice-22+25 `hook::Bus*` tap (publishes `tool_before` after the registry resolves the tool def, `tool_dispatched` between audit success and the handler co_await on the paths where the handler runs — `allow` or ask-approved, carrying the verdict wire spelling, `tool_error` on every error exit as a failure-only narrow channel, and `tool_after` at every exit — all four advisory), built-ins `file.read` (`tool::register_file_read`, capability `read_file`; resolves through `tool::Workspace` when supplied), `file.write` (`tool::register_file_write`, capability `write_file`, input `{path, content, mode?, create_parents?, max_bytes?}` with `mode ∈ {truncate, append, fail_if_exists}` and 16 MiB default / hard cap; resolves through `tool::Workspace` when supplied), `file.edit` (`tool::register_file_edit`, capability `edit_file`, input `{path, old_string, new_string, replace_all?, max_bytes?}` with the read + final output capped at the same 16 MiB default / hard cap; resolves through `tool::Workspace` when supplied), `file.search` (`tool::register_file_search`, capability `read_file`, input `{path, pattern, max_matches?, include_hidden?, regex?}` — literal substring by default; `regex=true` (slice 24) compiles via `permission::InputPattern` and matches per line via re2 `PartialMatch` with `regex_error` on compile failure; single-file or recursive directory walk; binary NUL-skip; dotfile-skip by default), `directory.list` (slice 29, `tool::register_directory_list`, capability `list_directory`, input `{path, include_hidden?, max_entries?}` — single-level enumeration through `oran-io::list_directory`; one `<path>:<kind>:<size_bytes or '-'>` line per entry sorted by path, `no entries` when empty, `io: directory entry limit exceeded` propagated verbatim on overflow), and `file.delete` (slice 30, `tool::register_file_delete`, capability `delete_path`, input `{path}` — refuses non-regular files (directories and symlinks reject as `invalid_argument`), `not_found` when the file does not exist, success returns `deleted <path>`; resolves through `tool::Workspace` when supplied); planned blocking-veto semantics on `tool_before`, and the rest of the built-in catalog | `oran-core`, `oran-async`, `oran-permission`, `oran-hook`, `oran-io` |
| `oran-hook`          | hook bus + sink kinds: `Event` enum (41 lifecycle events), `Sink` abstract interface, `Bus` with `bind` / `unbind` / `publish_advisory` (advisory contract — sinks observe, never veto; sink exceptions are caught and surfaced as `Error::internal` in `PublishOutcome`), `InProcessSink` (std::function-backed); planned `ShellSink` / `WebhookSink` / `LuaSink` (`sol2` feature-gated) and blocking-veto `publish_blocking` overload | `oran-core`, `oran-async` |
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
| `oran-bootstrap`     | process entry + config loading + CLI handoff + `--explain-rules` (with `--mode` / `--agent` selectors) + `--audit-init` + per-process `RuntimeAssembly` (bundles a fresh `permission::ApprovalBroker` and the active `permission::AuditSink`; defaults to `audit_enabled=true` now that the migrations ship inside the binary) + the slice-23 `SignalScope` SIGINT/SIGTERM trap (`asio::signal_set` RAII, `release()` cancel + `signum()` capture) that `--audit-init` adopts so Ctrl-C / `kill` interrupt the one-shot `io_context` drain promptly, with `bootstrap::run` translating the resulting `Error::cancelled` into the shell-conventional `128 + signum` exit code | currently `oran-core`, `oran-async`, `oran-storage`, `oran-config`, `oran-permission`, `oran-cli`; planned every public lib above |

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
  `session`, `web`, `permissions`, and `agents.<name>.permissions`; planned
  sections such as channels, teams, hooks, memory, automation, and the
  remaining `agents.<name>.*` fields (provider/model override, prompts,
  hook bindings) are accepted as recognized root fields until their typed
  models land.
- `${VAR}` and `${VAR:-default}` substitutions run on string values at load time.
- Secret encryption, generated JSON Schema, and rotation remain planned follow-up
  slices. See `docs/design-docs/secrets-and-state.md`.

## To Fill In As The Project Matures

This file should grow with the project but stay scannable. The expected next edits are:

- Datapath diagrams per channel adapter once the first three adapters land.
- Identity / scope diagram once memory tiers ship.
- Observability stack diagram (logs / metrics / traces) once shipping.
- Deployment topology once a real runtime target exists.
