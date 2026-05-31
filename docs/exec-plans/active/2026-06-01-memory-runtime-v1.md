# Memory Runtime V1

## Goal

Land the first real `oran-memory` runtime path without collapsing the memory
tiers into the agent loop. The end state for this plan is: session conversation
memory has a typed library owner over `storage::SessionRepository`; bootstrap can
assemble the storage needed for configured-route sessions; `AgentPromptRunner`
persists and reloads conversation history through that owner; and the prompt
memory slot remains a once-per-turn input that can later consume long-term
memory search results without re-querying inside each ReAct iteration.

## Scope

- In scope:
  - Add the `oran-memory` library, umbrella header, tests bucket, and bench
    bucket parity.
  - Implement `memory::session::Store` as the typed wrapper over the existing
    `storage::SessionRepository`.
  - Serialize and deserialize `core::Message` / `core::Content` values at the
    memory layer, keeping `oran-storage` JSON-opaque.
  - Extend `RuntimeAssembly` with a sessions DB path/pool/repository that is
    separate from `audit.db`.
  - Wire configured-route `AgentPromptRunner` to load persisted session history
    before a prompt and append new user/assistant/tool messages after a
    successful turn.
  - Keep prompt memory framing a stable once-per-turn string input; later
    long-term memory search can fill it without changing `prompt::Builder`.
  - Update docs, status, quality, history, and release notes per
    `docs/rules/docs-in-sync.md`.
- Out of scope:
  - Long-term FTS5 records, decay, vector search, or the MEMORY.md mirror.
  - Team/shared memory.
  - New memory tools such as `memory.recall` / `memory.write`.
  - Hook payload implementation for memory read/write events beyond preserving
    the existing event names.
  - CLI commands for session selection/listing.

## Context

- Relevant docs:
  - `docs/design-docs/memory-system.md`
  - `docs/product-specs/0005-memory-system.md`
  - `docs/product-specs/0001-core-react-loop.md`
  - `docs/product-specs/0017-fake-provider-first-agent-loop.md`
  - `docs/design-docs/agent-platform.md`
  - `docs/design-docs/bootstrap-runtime.md`
  - `docs/design-docs/secrets-and-state.md`
  - `docs/rules/docs-in-sync.md`
  - `docs/rules/compile-budget.md`
- Relevant code paths:
  - `include/oran/storage/session_repository.hpp`
  - `src/oran-storage/session_repository.cpp`
  - `src/oran-storage/migrations/sessions/0001-sessions-initial.sql`
  - `include/oran/core/message.hpp`
  - `include/oran/core/content.hpp`
  - `include/oran/prompt/builder.hpp`
  - `include/oran/bootstrap/runtime_assembly.hpp`
  - `src/oran-bootstrap/runtime_assembly.cpp`
  - `include/oran/bootstrap/prompt_runner.hpp`
  - `src/oran-bootstrap/prompt_runner.cpp`
  - `src/oran-agent/loop.cpp`
  - `xmake/targets.lua`
- Constraints:
  - Memory reads are once per turn; never re-query inside each provider/tool
    iteration.
  - `oran-storage` stays JSON-opaque and role-typed; message JSON belongs in
    `oran-memory`.
  - Session DB is separate from audit DB: `<workspace>/.orangutan/sessions.db`.
  - Configured-route no-prompt REPL and `--prompt` share the same runner-owned
    session identity; built-in no-route defaults stay deterministic and avoid
    storage/provider credentials.
  - Public headers must avoid heavy JSON/asio/sqlite includes.
  - Focused slices should remain small; this plan exists because the end state
    crosses multiple libraries.
- Compile-budget impact:
  - Add `oran-memory` under the existing memory category budget
    (1.2 s median / 2.5 s p95 / 3.0 s hard cap).
  - Keep JSON serialization in `.cpp` files and prefer small TUs if the wrapper
    grows beyond session memory.

## Risks

- Risk: Persisting transcript messages twice or in the wrong order.
  Mitigation: first wire persistence at the `AgentPromptRunner` boundary, diff
  the returned transcript suffix against the already-loaded prefix, and test a
  two-prompt runner session.
- Risk: Storage errors after a successful provider turn create an ambiguous user
  response.
  Mitigation: make append failures propagate from `run_prompt` before the CLI
  reports success; the provider/tool trace rows remain audit evidence for the
  attempted turn.
- Risk: JSON shape leaks into `oran-storage` or provider adapters.
  Mitigation: keep JSON encode/decode private to `oran-memory`; test malformed
  stored rows through `memory::session::Store`.
- Risk: `RuntimeAssembly` becomes a catch-all for every future memory backend.
  Mitigation: only assemble the sessions repository in this plan; long-term
  `memory.db` gets its own follow-up owner once the FTS5 backend lands.
- Risk: New library parity increases build/test churn.
  Mitigation: add minimal tests and a simple session-store bench in the first
  library slice; expand benches only when long-term search introduces real
  alternatives.

## Milestones

1. **Plan and first boundary.**
   Create this active plan, update `STATUS.md`, and confirm the first slice is
   `oran-memory::session::Store`.
2. **Session store library.**
   Add `oran-memory`, typed message serialization, `memory::session::Store`,
   tests for round-trip/order/malformed rows, and bench parity.
3. **Runtime assembly sessions DB.**
   Add `RuntimeAssemblyOptions::sessions_db_path` / session enablement, build a
   sessions pool/repository over the shared executor, run migrations, and expose
   the repository/store to bootstrap owners.
4. **Configured-route runner persistence.**
   Have `AgentPromptRunner` load session history into the conversation tail and
   append only the new transcript suffix after a successful turn. Keep the
   existing in-memory transcript path as a fallback for session persistence
   disabled in tests.
5. **Prompt memory slot follow-up.**
   Introduce a minimal once-per-turn memory framing owner only after session
   persistence is stable; long-term recall remains outside this plan unless the
   docs are amended.

## Validation

- Commands:
  - `xmake build oran-memory`
  - `xmake run test-memory`
  - `xmake run bench-memory`
  - `xmake run test-bootstrap`
  - `xmake run test-agent`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `git diff --check`
  - `make ci`
- Manual checks:
  - Confirm `config.example.json` still loads without requiring memory secrets.
  - Confirm built-in no-route startup does not open provider credentials or
    require a sessions DB.
  - Confirm configured-route `AgentPromptRunner` session identity is stable
    across prompts in one process.
- Observability checks:
  - Session writes should not appear in `audit.db`; audit remains for effectful
    tool/provider actions.
  - Trace rows keep the same `session_id` semantics already used by
    `TraceRepository`.
- Bench comparison:
  - First session-store bench may compare raw `SessionRepository` append/load to
    typed `memory::session::Store` append/load. Long-term FTS5/vector benchmarks
    stay out of scope.

## Progress Log

- [x] 2026-06-01 04:32 +0800: Re-read `STATUS.md`, core repo rules,
  memory/CLI/agent/bootstrap docs, and the live storage/prompt/runner code.
- [x] 2026-06-01 04:32 +0800: Chose memory over CLI line editor/history because
  session persistence is part of the MVP runtime and currently has only storage
  foundation plus a static prompt slot.
- [x] 2026-06-01 04:46 +0800: Landed milestone 2 as slice 130:
  `oran-memory::session::Store` over `storage::SessionRepository`, private
  `core::Message` JSON serialization, `test-memory` 5 / 550, and
  `bench-memory` raw repository vs. typed store parity.
- [x] 2026-06-01 05:13 +0800: Landed milestone 3 as slice 131:
  `RuntimeAssembly` now opens/migrates a separate
  `<workspace>/.orangutan/sessions.db`, owns the session pool/repository/store,
  exposes `session_store()` / `sessions_path()`, enables that path for
  configured routes, and keeps built-in no-route startup disabled for session
  memory. Focused validation: `test-bootstrap` 80 / 422.
- [ ] Land milestone 4 after runtime assembly exposes the sessions owner.
- [ ] Refresh docs/status/quality/history/release notes in every slice.

## Decision Log

- 2026-06-01: Start with session memory, not long-term FTS5. The repository
  already has `storage::SessionRepository`, `core::Message`, and a runner-owned
  transcript; the smallest useful memory slice is the typed wrapper and its JSON
  boundary. Long-term memory would add schema/search/hook/retention concerns
  before the configured-route loop can even persist conversations.
- 2026-06-01: Keep message serialization in `oran-memory`. `oran-storage`
  deliberately stores `content_json` / `metadata_json` opaquely while typing
  `core::Role`; moving message JSON there would blur the layer boundary the
  memory design doc calls out.
- 2026-06-01: Assemble sessions separately from audit/trace. `sessions.db` is a
  product data store, not audit evidence, and the memory design doc explicitly
  splits database files to avoid contention.

## Linked Artifacts

- Related design doc: `docs/design-docs/memory-system.md`
- Related design doc: `docs/design-docs/bootstrap-runtime.md`
- Related product spec: `docs/product-specs/0005-memory-system.md`
- Related product spec: `docs/product-specs/0001-core-react-loop.md`
- PRs: TBD
- History entries:
  - `docs/histories/2026-06/20260601-0446-memory-session-store.md`
  - `docs/histories/2026-06/20260601-0513-runtime-session-memory.md`
- Release notes:
  - `docs/releases/feature-release-notes.md` (`memory-session-store`)
  - `docs/releases/feature-release-notes.md` (`runtime-session-memory`)
