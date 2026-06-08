## [2026-06-08 14:01] | Task: automation agent prompts

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local CLI
- Linked plan: `docs/exec-plans/active/2026-06-07-automation-cron-category.md`

### User Query

> Continue the automation workstream, do not blindly follow STATUS, and make the
> current slice materially useful. The user also confirmed pre-v1 automation DB
> compatibility is not required because generated artifacts do not exist yet.

### Changes Overview

- Areas: `oran-automation`, `oran-config`, `oran-bootstrap`, automation docs.
- Key actions: made cron and triggered job descriptors carry required
  non-empty `agent_prompt`, mapped config-authored cron prompts into bootstrap
  seed descriptors, and updated the pre-v1 automation migrations in place.

### Design Intent

Cron and triggered automation jobs already carried durable job/agent keys, but
they still lacked the actual prompt text the future agent-firing adapter must
pass into `cli::PromptRunRequest::prompt`. This slice makes stored descriptors
executable inputs before adding notifier routing or `AgentPromptRunner`
ownership. Because automation schema artifacts have not been generated or
released, the base cron/triggered migrations were updated directly instead of
adding compatibility migrations.

### Files Modified

- `include/oran/automation/repository.hpp`
- `include/oran/config/config.hpp`
- `src/oran-automation/repository.cpp`
- `src/oran-automation/migrations/automation/0003-automation-cron-jobs.sql`
- `src/oran-automation/migrations/automation/0007-automation-cron-agent-leases.sql`
- `src/oran-automation/migrations/automation/0008-automation-triggered-jobs.sql`
- `src/oran-config/config.cpp`
- `src/oran-bootstrap/automation_cron.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `config.example.json`
- `tests/automation/test_repository.cpp`
- `tests/automation/test_service.cpp`
- `tests/automation/test_queue.cpp`
- `tests/automation/test_runtime.cpp`
- `tests/bootstrap/test_bootstrap.cpp`
- `tests/bootstrap/test_memory_retention.cpp`
- `tests/bootstrap/test_runtime_assembly.cpp`
- `tests/config/test_config.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

List every doc edited in the same PR as part of this change. If the change
invalidated a doc and the matching edit is *missing*, the PR is incomplete.

- `docs/design-docs/automation-runtime.md` — cron/triggered descriptor API,
  config seed, and migration semantics now include required `agent_prompt`.
- `docs/product-specs/0006-automation.md` — shipped prework, implementation
  status, and acceptance notes now describe prompt-bearing descriptors.
- `docs/ARCHITECTURE.md` — config/storage/automation inventory reflects the
  required automation prompt field.
- `docs/QUALITY_SCORE.md` — updated automation/config/bootstrap assertion
  counts and slice coverage note.
- `docs/STATUS.md` — bumped to slice 219 and repointed the latest history.
- `docs/releases/feature-release-notes.md` — added the user-visible release
  note for prompt-bearing automation jobs.
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md` — recorded
  the plan progress and linked history.

### Validation

- Commands run:
  - `git diff --check`
  - `xmake build oran-automation`
  - `xmake build oran-config`
  - `xmake build oran-bootstrap`
  - `xmake build orangutan`
  - `xmake build test-config`
  - `xmake build test-bootstrap`
  - `xmake build test-automation`
  - `build/linux/x86_64/release/test-config "[automation]"`
  - `build/linux/x86_64/release/test-bootstrap "[automation]"`
  - `build/linux/x86_64/release/test-automation "[repository]"`
  - `build/linux/x86_64/release/test-automation "[service]"`
  - `build/linux/x86_64/release/test-automation "[queue]"`
  - `build/linux/x86_64/release/test-automation "[runtime]"`
  - `xmake run test-config`
  - `xmake run test-bootstrap`
  - `xmake run test-automation`
  - `xmake run orangutan -- --help`
  - `make ci`
- Tests added/changed: config rejects missing/empty cron `agent_prompt`;
  repository rejects empty cron/triggered prompts and round-trips prompt
  updates; service/queue/runtime/bootstrap tests assert prompt preservation.
- Bench impact: none; schema/config contract change only.
- Compile-budget delta: no new third-party dependency or translation unit.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`.
