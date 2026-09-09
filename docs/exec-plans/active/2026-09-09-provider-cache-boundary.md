# Provider Cache Boundary

## Objective

Complete the provider cache-control handoff while removing the provider's
dependency on prompt rendering. A turn supplies an owned stable-prefix identity;
the selected protocol target applies its own policy and emits supported cache
fields. Primary policy must not discard information needed by later fallbacks.

## Current Contracts

- [Provider](../../design-docs/api-portability.md) owns request mapping, retry and
  fallback. Its encoders currently ignore cache hints.
- [Prompt design](../../rules/prompt-design.md) owns stable text and native-tool
  fingerprints. The provider currently validates its five-section layout and
  copies section metadata that no protocol consumes.
- [Architecture](../../ARCHITECTURE.md) currently permits provider to depend on
  prompt solely for that mapping.
- The loop filters hints using primary policy before fallback selection. A
  disabled primary or a stricter byte floor therefore also suppresses otherwise
  eligible fallback controls.

## Complete Slice

1. Retain only provider-owned prefix hash/byte values. Remove section-copying
   mapping, its unused result duplication and the provider-to-prompt dependency.
2. Pass prefix values from the loop without applying route policy. Preserve them
   across retries and fallbacks; apply the selected target's enabled flag and
   byte floor when encoding its request.
3. Encode Anthropic's ephemeral breakpoint on the stable system text, or the
   last native tool when no stable system text exists. Lifted conversation system
   messages and tool results must not extend that explicit breakpoint.
4. Encode OpenAI Responses' prefix routing key. Disabling client controls omits
   the key; it does not promise to disable the service's automatic caching.
5. Replace obsolete mapping tests/bench fixtures with protocol and composed
   runtime coverage; update owning contracts and the handoff, then delete this
   completed plan.

Protocol references: [OpenAI prompt caching](https://developers.openai.com/api/docs/guides/prompt-caching)
and [Anthropic prompt caching](https://platform.claude.com/docs/en/build-with-claude/prompt-caching).
Validate available official documentation and record any limits on verification.

## Bounds And Risks

No database, message encoding, permission, tool authority or async ownership
changes. No new library, dependency, configuration option, TTL policy or claim
of live cache hits. Public removal of the old section-mapping API needs a concrete
migration note. Cache values must describe only stable system text and native
tools; dynamic conversation remains separate. Local byte floors do not substitute
for provider token thresholds. Removing prompt includes reduces public coupling;
compile-budget thresholds are unchanged.

## Verification

- Protocol payloads: enabled, disabled, equal/under-floor, missing/empty prefix,
  stable system text, tools-only prefix and dynamic system messages.
- Composed loop/transport: retry and fallback use each target's own policy,
  including a disabled primary followed by an eligible fallback.
- Prompt fingerprints remain stable across conversation changes and change with
  native schema bytes; provider sources/headers no longer import prompt.
- Affected provider/agent/bootstrap tests, full release build and test suite,
  affected benchmark builds, and `make ci` before the implementation commit.

## Progress

- [x] Read handoff and contracts; merge and remove the previous branch.
- [x] Identify the policy-loss defect and obsolete cross-library mapping.
- [ ] Implement the complete boundary and regression coverage.
- [ ] Update contracts, pass verification and remove this plan.
