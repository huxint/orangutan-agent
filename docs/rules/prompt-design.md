# Runtime Prompt Design

[prompt::render](../../include/oran/prompt/render.hpp) is a synchronous value
transformation over core messages, tool values and caller text. It needs no
registry, configuration, executor or cache owner. Stable inputs produce
identical bytes across provider/tool iterations.
This rule governs runtime prompts, not development-agent routing instructions.

## Text Section Order

1. System preamble: identity, operating principles and response contract.
2. Optional caller-supplied skill catalogue.
3. Scoped memory index or exact caller framing selected at the prompt boundary.
4. Stable per-agent instructions.
5. Conversation messages, including the current user and tool results.

Sections 1–4 form the stable text prefix supplied to the provider as system text.
Conversation is dynamic. Clocks, request IDs, trace IDs, counters and status
narration stay out of stable sections. Bump section versions when rendering rules
change; avoid per-request version churn.

## Native Tools And Cache Identity

The loop selects one owned, sorted tool catalogue for the entire turn through
`tool::select_tools`. The [tool contract](../design-docs/tool-runtime.md) owns
selection and authority. Both supported protocols receive descriptions and JSON
schemas through `provider::Request::tools`; system text contains neither a copy
of the schemas nor a deferred-tool index. Child sessions omit disabled delegation
from both their available catalogue and explicit selection.

The renderer receives the exact ordered native definitions to fingerprint their
names, descriptions and opaque schema bytes. Required capabilities remain local
permission metadata. Field lengths preserve hash boundaries. The tool fingerprint
and its cache version join stable text in `RenderedPrompt::prefix_hash`, so a
schema, description, selection or order change invalidates the effective prefix
without adding tool text. Conversation changes leave that identity stable.

`prefix_bytes` counts stable text and native name/description/schema bytes. It
excludes protocol framing and is neither a token count nor serialized request
size. The loop copies the hash and byte count into provider-owned request values;
section IDs and versions stay in the prompt layer. The unused
`CacheSection::is_breakpoint` flag is removed; protocol adapters place controls
at the end of the submitted stable system text or native tools. The
[provider contract](../design-docs/api-portability.md) owns per-target eligibility
and protocol controls, independent of this section layout.

## Memory Context

Bound MemoryRecall, MemoryRemember and MemoryForget tools are directly available
by default. The memory section is a bounded index of IDs, titles and content
cues. Complete notes enter the dynamic conversation through model-directed reads.
Scores, read times and mutable counters stay out of index text. Index loading
happens once before the loop, so tool iterations reuse the prefix and accepted
writes change the next prompt's index. The preamble defines consultation and
same-turn durable learning triggers; [memory-system](../design-docs/memory-system.md)
owns their semantics and the distinction between guidance and runtime guarantees.

`bench-prompt` exercises pure rendering with full and reduced native catalogues.
`scripts/check-prompt-preamble.sh` checks the default system preamble for dynamic
inputs; behavioral tests check bytes, tool fingerprints and cache invalidation.

For a new model-visible surface, consult the relevant proven shape in
<https://github.com/Piebald-AI/claude-code-system-prompts>, then record the adopted
contract in its owning design document.
