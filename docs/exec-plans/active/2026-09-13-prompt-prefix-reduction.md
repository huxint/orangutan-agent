# Prompt Prefix Reduction

## Goal

Render one owned stable prefix per turn and send conversation only through typed
provider messages. Remove diagnostic conversation projection, section copies and
repeated rendering without changing system text, cache identity or persistence.

## Scope

- Reduce `prompt::RenderInputs` to stable text and native tools. Remove the
  conversation version and input, `CacheSection`, per-section hashes and the
  redundant native-byte subtotal from the public result.
- `RenderedPrompt` owns joined `system_prompt` text and the existing prefix
  hash/byte count and native-tool fingerprint. Keep stable-section ordering,
  empty-section joining, version defaults and fingerprint encoding unchanged.
- Select tools and render once before provider/tool iteration. Reuse that value
  for requests, traces and the return value. Delete the loop's section joiner
  and `last_rendered` copy. Keep conversation in requests and `transcript`.
- Migrate tests and document the removed diagnostic interfaces. Preserve
  provider retries/fallbacks, tool authority, cancellation joins, zero-attempt
  behavior and all stored data/schema encodings.
- Benchmark pure prefix rendering and rebuilding versus reuse over eight
  iterations. No process-wide cache, new provider policy or storage change.

## Contracts And Risks

[Prompt design](../../rules/prompt-design.md) owns rendering and fingerprint
compatibility; [agent execution](../../design-docs/agent-platform.md) owns turn
values and traces. Provider receives its existing hash/byte values. The byte
count retains its existing section-content semantics, excluding join separators.

Returned diagnostics change: hosts read `rendered_prompt.system_prompt` for
submitted system text and `transcript` for conversation. Preserve provider text
and cache keys with fixed pre-change fixtures, including empty and separated
sections. Caller edits during a turn must not alter the owned prefix; the next
turn observes them. Existing cancellation and trace regressions cover the same
awaited effect boundaries; their ownership is unchanged.

The slice spans public headers, runtime callers, tests, benchmarks and owning
contracts, so it exceeds the six-file review guideline. It removes dependencies
on message rendering without adding templates or third-party dependencies.
Compile budgets and release LTO remain unchanged; no compile-cost claim is made.

## Verification And Completion

- [x] Confirm renderer fields and all runtime/test/benchmark consumers.
- [ ] Capture compatible prefix fixtures and a release benchmark baseline.
- [ ] Reduce rendering and make one prefix snapshot per turn.
- [ ] Verify joining/ownership/version behavior, changing conversation,
  per-turn refresh, protocol payloads and trace attribution.
- [ ] Run release library build and all 14 test targets; build/run bench-prompt
  and build bench-agent; run `make ci` and inspect the final diff.
- [ ] Update owning contracts, remove this completed plan and commit.

Use `xmake f -y -m release`, `xmake build -j4`, `xmake test -j4`,
`xmake build -j4 bench-prompt`, `xmake run bench-prompt`,
`xmake build -j4 bench-agent` and `make ci`. Targeted iteration uses `test-prompt`
and `test-agent`. Compare benchmark results under the same compiler, mode and
hardware, with local frequency-scaling limits stated in the change description.
