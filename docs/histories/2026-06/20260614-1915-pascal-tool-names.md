## [2026-06-14 19:15] | Task: PascalCase tool names

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: local `orangutan-refactor` workspace
- Linked plan: none; operator requested an immediate global rename after the
  Anthropic-compatible endpoint rejected dotted tool names.

### User Query

> Rename tools directly instead of keeping dotted names or disabling active
> tools; use normal first-letter-uppercase tool names.

The preceding live-provider diagnostics involved user-supplied API keys; this
history intentionally records only the protocol symptom and omits secrets.

### Changes Overview

- Areas: `oran-tool` built-ins, prompt catalog, provider request/response test
  fixtures, permissions/config examples, bootstrap/agent runners, and current
  docs.
- Key actions: renamed shipped tool names from dotted spellings to PascalCase
  (`FileRead`, `ToolSearch`, `MemoryRecall`, `SkillInvoke`, and the rest of the
  built-ins), updated fake test-tool fixtures away from dotted placeholders, and
  kept provider adapters using `ToolDef::name` directly so Anthropic-family
  tool requests are valid without a second alias map.

### Design Intent

Anthropic-family tool names must be alphanumeric/underscore/hyphen names, and a
tested Anthropic-compatible endpoint rejected dotted names before the model can
call a tool. A provider-only alias layer would have preserved the old
internal names but left prompts, permissions, audits, and transcript state using
names that cannot be sent as-is to a real provider. This slice makes the public
tool name itself provider-safe, so the registry, prompt builder, agent loop,
permission patterns, audit rows, and protocol adapters all share one spelling.

### Files Modified

- `include/oran/tool/builtins.hpp` - canonical built-in tool-name constants now
  use PascalCase.
- `src/oran-tool/*`, `src/oran-agent/*`, `src/oran-bootstrap/*`,
  `src/oran-prompt/*`, and `src/oran-skill/*` - messages, catalog handling,
  runner glue, and diagnostics now refer to the new public names.
- `tests/**` and `bench/**` - expected tool names, permission patterns,
  provider fixtures, fake tools, and examples now use PascalCase tool names.
- `config.example.json` - sample permission rules now use provider-safe tool
  names.

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/tool-runtime.md` - tool-definition examples and shipped
  built-in catalog names now describe PascalCase names.
- `docs/design-docs/api-portability.md` - provider-portability docs now reflect
  provider-safe tool names.
- `docs/product-specs/0001-core-react-loop.md`,
  `docs/product-specs/0002-tool-registry.md`,
  `docs/product-specs/0014-structured-tool-output.md`,
  `docs/product-specs/0016-prompt-and-tool-catalog-cache.md`, and related
  current specs - tool examples now use the new public names.
- `docs/ROADMAP.md` and `docs/STATUS.md` - slice 245 frontier and validation.
- `docs/releases/feature-release-notes.md` - user-visible release note.

### Validation

- Commands run:
  - `git diff --check`
  - `xmake build test-provider && xmake run test-provider`
  - `xmake build test-tool && xmake run test-tool`
  - `xmake build test-permission && xmake run test-permission`
  - `xmake build test-config && xmake run test-config`
  - `xmake build test-prompt && xmake run test-prompt`
  - `xmake build test-agent && xmake run test-agent`
  - `xmake build test-bootstrap && xmake run test-bootstrap`
  - `xmake build test-http && xmake run test-http`
  - `xmake test`
  - `make ci`
  - `xmake run orangutan -- --help`
  - final residual-name scan for old dotted built-in tool names outside
    `docs/histories/**` and `docs/references/**`
- Tests added/changed: existing provider/tool/permission/prompt/agent/bootstrap
  assertions now pin PascalCase tool names across request JSON, streamed tool
  starts, prompt catalogs, permissions, audit/trace output, and runner fixtures.
  `test-http` now pins loopback `NO_PROXY` for the "nothing listens" WebSocket
  case so local proxy settings cannot turn a refused loopback dial into a
  successful proxied handshake during full-suite validation.
- Bench impact: no new benchmark; bench fixtures were renamed so they no longer
  use dotted fake tool names.
- Compile-budget delta: not measured; no new dependencies or heavy public
  includes were added.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md#2026-06`.
