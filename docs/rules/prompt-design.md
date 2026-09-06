# Runtime Prompt Design

Render prompt sections as pure functions of their inputs. Stable content must
produce identical bytes across provider/tool iterations. This rule governs the
prompts emitted by Orangutan, not development-agent routing instructions.

## Section Order

1. System preamble: identity, operating principles and response contract.
2. Active tool catalogue: registered names, descriptions and JSON schemas.
3. Deferred-tool index: compact names and descriptions.
4. Optional caller-supplied skill catalogue.
5. Memory framing selected once at the prompt boundary.
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
and explicit active-tool selection. Memory framing derives from selected records and excludes lookup
scores and retrieval timestamps. Recall happens once before the loop, so later
tool iterations reuse the same prefix.

Provider adapters map the resulting sections into their protocol's cache hints.
`bench-agent` compares stable-prefix behavior across changing conversation tails.
`scripts/check-prompt-preamble.sh` checks the default system preamble for dynamic
inputs; behavioral tests check bytes and hashes.

For a new model-visible surface, consult the relevant proven shape in
<https://github.com/Piebald-AI/claude-code-system-prompts>, then record the adopted
contract in its owning design document. Existing reductions preserve section
membership rather than creating new prompt surfaces.
