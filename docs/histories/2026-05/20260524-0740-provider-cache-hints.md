## [2026-05-24 07:40] | Task: provider cache hints

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `local workspace / xmake`
- Linked plan: none — `docs/STATUS.md` named a narrow provider cache-mapping
  follow-up, and this slice stayed below the exec-plan threshold.

### User Query

Continue advancing the rewrite after agent session promotion, keeping one coherent
version per commit and syncing docs with the code.

### Changes Overview

- Areas: `oran-provider`, provider tests, provider benches, prompt/provider docs,
  build dependency hygiene.
- Key actions: added the first `oran-provider` target, exported provider-domain
  `Request` / `Response` values, added `PromptCacheHints` / `PromptCacheOptions`,
  mapped `prompt::RenderedPrompt` sections 1-6 into adapter-facing cache keys,
  rejected malformed prompt-cache boundaries, skipped disabled or too-small
  cache routes, and registered `test-provider` plus `bench-provider`.

### Design Intent

Spec 0016 already made `prompt::RenderedPrompt` the cache-boundary contract, but
the provider side still had no code surface to consume it. This slice chooses the
smallest stable bridge: a pure cache-hint mapper plus value types, not a transport,
protocol adapter, retry runtime, or fake provider. The mapper validates the prompt
shape before an adapter can place a vendor cache marker, and it keeps the
conversation tail out of the cache keys by construction. The intentional
`provider -> prompt` sibling dependency is one-way: providers consume rendered
prompt metadata, while prompt construction never calls providers.

### Files Modified

- `include/oran/provider.hpp`
- `include/oran/provider/cache.hpp`
- `include/oran/provider/types.hpp`
- `src/oran-provider/cache.cpp`
- `tests/provider/`
- `bench/provider/`
- `xmake/targets.lua`
- `xmake/tests.lua`
- `xmake/bench.lua`
- `scripts/check-deps.sh`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice/history pointer, provider status, latest provider test
  count, and next intended work updated.
- `docs/ARCHITECTURE.md` — `oran-provider` inventory now reflects the shipped
  domain/cache-hint surface and its current dependencies.
- `docs/BUILD_SYSTEM.md` — target inventory now includes `oran-provider` and notes
  that provider is not linked into the binary yet.
- `docs/design-docs/api-portability.md` — provider domain model and cache mapping
  now name the shipped `PromptCacheHints` helper.
- `docs/design-docs/agent-platform.md` — prompt assembly status now treats provider
  cache hints as shipped while keeping the fake loop future.
- `docs/design-docs/tool-runtime.md` and
  `docs/product-specs/0014-structured-tool-output.md` — structured tool-result
  protocol mapping remains future, but it is no longer blocked on provider target
  existence.
- `docs/design-docs/module-boundaries.md` — live sibling dependency exceptions now
  include `provider -> prompt`.
- `docs/product-specs/0010-benchmark-harness.md`,
  `docs/rules/testing-and-bench.md`, and `bench/README.md` — provider benchmark
  coverage now reflects cache-hints enabled vs. disabled.
- `docs/product-specs/0016-prompt-and-tool-catalog-cache.md` — provider-side
  breakpoint validation and cache-hint acceptance status updated.
- `docs/product-specs/0017-fake-provider-first-agent-loop.md` — provider value
  shapes are now shipped before `FakeProvider`.
- `docs/QUALITY_SCORE.md` — test, bench, provider, agent, and prompt rows updated.
- `tests/README.md` — `tests/provider` marked live.
- `docs/releases/feature-release-notes.md` — release note added.

### Validation

- Commands run:
  - `xmake build oran-provider`
  - `xmake build test-provider`
  - `xmake run test-provider`
  - `xmake build bench-provider`
  - `xmake run bench-provider`
- Tests added/changed: `test-provider` reports 3 cases / 25 assertions.
- Bench impact: `bench-provider` reports `provider.cache_hints_enabled` about
  386 ns / mapping and `provider.cache_hints_disabled` about 317 ns / mapping.
- Compile-budget delta: no budget row changed; `oran-provider` already has a
  documented provider/prompt/agent TU category, and the new headers keep heavy
  vendor/JSON types out of the public surface.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`
  (`provider-cache-hints`).
