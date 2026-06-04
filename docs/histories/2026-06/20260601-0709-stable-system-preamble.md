## [2026-06-01 07:09] | Task: stable system preamble

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local CLI in `local repository checkout`
- Linked plan: none

### User Query

Continue the prompt-runtime arc after memory framing by landing the first stable
system-preamble template and the prompt-design grep follow-up.

### Changes Overview

- Areas: `oran-agent`, `oran-bootstrap`, prompt-runtime tests, scripts, docs/status,
  release notes.
- Key actions:
  - Added `agent::SystemPreamble`, `agent::SystemPreambleOwner`, and
    `agent::default_system_preamble()` as the versioned section-1 prompt owner.
  - Had `agent::Loop` use its owned default when callers leave
    `RunTurnInputs::system_preamble` empty.
  - Had `AgentPromptRunner` render the stable system preamble once before loop
    entry and expose `system_preamble_renders()` for diagnostics.
  - Updated the prompt-cache stability bench fixture to use the repository
    default preamble bytes.
  - Added `scripts/check-prompt-preamble.sh` and wired it into `scripts/ci.sh`
    so clocks, ids, and cross-section prompt bytes cannot drift into the
    default preamble unchecked.
  - Bumped the binary slice tag to `2.0.0-slice134`.

### Design Intent

Section 1 is the cached prompt prefix's root contract, so it needs a small,
versioned owner before more prompt surfaces accrete around it. The default text
is deliberately narrow: agent identity, tool/effect honesty, permission/hook
authority, error reporting, secret handling, and concise response shape. Tool
catalog bytes, memory framing, skills, per-agent overlays, and conversation
history remain in their documented sections so cache invalidation stays local.

The Piebald Claude Code system-prompt corpus was consulted for prior art. This
slice adopts the stable multi-part prompt separation and rejects copying a large
Claude-specific system prompt because Orangutan's section 1 should be a minimal
runtime contract while tools, memory, skills, and tail state stay out of it.

### Files Modified

- `include/oran/agent/system_preamble.hpp`
- `include/oran/agent.hpp`
- `include/oran/agent/loop.hpp`
- `src/oran-agent/system_preamble.cpp`
- `src/oran-agent/loop.cpp`
- `include/oran/bootstrap/prompt_runner.hpp`
- `src/oran-bootstrap/prompt_runner.cpp`
- `bench/agent/scenarios/prompt_cache_hit_rate.cpp`
- `tests/agent/test_system_preamble.cpp`
- `tests/agent/test_loop.cpp`
- `tests/bootstrap/test_prompt_runner.cpp`
- `scripts/check-prompt-preamble.sh`
- `scripts/ci.sh`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — moved the project to slice 134, refreshed focused test
  counts, and set the next prompt-runtime slice.
- `docs/QUALITY_SCORE.md` — updated agent, bootstrap, prompt-builder, and test
  status with the shipped preamble owner.
- `docs/ARCHITECTURE.md` — updated the `oran-agent` and `oran-bootstrap`
  inventory rows for the new prompt owner and runner render counter.
- `docs/design-docs/agent-platform.md` — documented section-1 ownership and
  the adopted/rejected external prompt-corpus patterns.
- `docs/design-docs/bootstrap-runtime.md` — documented the runner's once-per-
  prompt system-preamble render.
- `docs/product-specs/0016-prompt-and-tool-catalog-cache.md` — added the
  system-preamble owner status and static grep gate.
- `docs/product-specs/0017-fake-provider-first-agent-loop.md` — recorded that
  empty loop inputs now select the loop-owned default preamble.
- `docs/rules/prompt-design.md` — replaced the planned grep note with the
  shipped mechanical enforcement.
- `docs/rules/docs-in-sync.md` — listed `scripts/check-prompt-preamble.sh` in
  mechanical enforcement.
- `docs/releases/feature-release-notes.md` — added the user-visible slice 134
  release note.

### Validation

- Commands run:
  - `scripts/check-prompt-preamble.sh`
  - `bash -n scripts/check-prompt-preamble.sh scripts/ci.sh`
  - `xmake build test-agent`
  - `xmake run test-agent`
  - `xmake build test-bootstrap`
  - `xmake run test-bootstrap`
  - `xmake build bench-agent`
  - `xmake run bench-agent`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `make ci`
  - `git diff --check`
- Tests added/changed:
  - `test-agent` adds direct `SystemPreambleOwner` coverage and a loop
    regression proving empty `RunTurnInputs::system_preamble` uses stable
    section-1 bytes across provider iterations and conversation-tail changes.
  - `test-bootstrap` adds a two-iteration runner case that asserts one
    system-preamble render before loop entry.
  - Focused results: `test-agent` 56 cases / 10 744 assertions;
    `test-bootstrap` 84 cases / 477 assertions.
- Bench impact:
  - The prompt-cache stability fixture now uses
    `agent::default_system_preamble()` instead of a local placeholder. The
    `bench-agent` bucket ran successfully; the prompt-cache scenarios reported
    `agent.prompt_cache_no_promotions` about 56.6 us / fixture and
    `agent.prompt_cache_after_promotion` about 63.0 us / fixture. No new
    benchmark scenario was added.
- Compile-budget delta:
  - No threshold changes; the new preamble TU is small and dependency-light.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`
