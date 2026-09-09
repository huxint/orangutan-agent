# Runtime Prompt Design

Render prompt sections as pure functions of their inputs. Stable content must
produce identical bytes across provider/tool iterations. This rule governs the
prompts emitted by Orangutan, not development-agent routing instructions.

## Section Order

1. System preamble: identity, operating principles and response contract.
2. Active tool catalogue: registered names, descriptions and JSON schemas.
3. Deferred-tool index: compact names and descriptions.
4. Optional caller-supplied skill catalogue.
5. Scoped memory index or exact caller framing selected once at the prompt boundary.
6. Stable per-agent instructions.
7. Conversation messages, including the current user and tool results.

Sections 1–6 form the cached prefix. The conversation is dynamic. Clocks, request
IDs, trace IDs, counters and status narration stay out of stable sections. A
changed stable input deliberately changes the content hash. Bump section versions
when rendering rules change; avoid per-request version churn.

Tool descriptions derive from `ToolDef`; catalogue rendering never grants a
capability. `prompt::is_default_active_tool` supplies the shared default selection
for cached catalogues and provider-native tool declarations. `AgentRun` is active
when registered; child sessions remove disabled delegation from their catalogue
and explicit active-tool selection. `MemoryRecall` and `MemoryRemember` are
active by default so the agent can inspect and record durable context without
first discovering a deferred tool. `MemoryForget` remains deferred. The default
memory section is a bounded index of IDs, titles and content cues. Complete notes
enter the dynamic conversation through model-directed reads. Scores, read times
and mutable counters stay out of index text. Index loading happens once before
the loop, so tool iterations reuse the prefix and accepted writes change the next
prompt's index. The preamble defines consultation and same-turn durable learning
triggers; [memory-system](../design-docs/memory-system.md) owns their semantics
and the distinction between model guidance and runtime guarantees.

Provider adapters map the resulting sections into their protocol's cache hints.
`bench-agent` compares stable-prefix behavior across changing conversation tails.
`scripts/check-prompt-preamble.sh` checks the default system preamble for dynamic
inputs; behavioral tests check bytes and hashes.

For a new model-visible surface, consult the relevant proven shape in
<https://github.com/Piebald-AI/claude-code-system-prompts>, then record the adopted
contract in its owning design document. Existing reductions preserve section
membership rather than creating new prompt surfaces.
