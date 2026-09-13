# Runtime Prompt Design

[prompt::render](../../include/oran/prompt/render.hpp) is a synchronous value
transformation over native tool values and stable caller text. It returns owned
system text and cache identity, with no registry, configuration, executor or
cache owner. The [agent loop](../design-docs/agent-platform.md) owns one rendered
prefix per turn.
This rule governs runtime prompts, not development-agent routing instructions.

## Stable System Text

1. System preamble: identity, operating principles and response contract.
2. Optional caller-supplied skill catalogue.
3. Scoped memory index or exact caller framing selected at the prompt boundary.
4. Stable per-agent instructions.

`RenderedPrompt::system_prompt` joins nonempty sections in that order with one
newline. It preserves their bytes, including whitespace and existing newlines;
all-empty input produces an empty string. Conversation, including user messages
and tool results, enters `provider::Request::messages` as typed content. The
renderer does not project conversation into a second text representation.

Clocks, request IDs, trace IDs, counters and status narration stay out of stable
sections. Bump section versions when model-visible rendering rules change;
removing diagnostic projections leaves system bytes and versions unchanged.

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

`prefix_bytes` retains the sum of stable section-content and native
name/description/schema bytes. It excludes the joining newlines and protocol
framing, preserving existing cache-policy thresholds; it is neither a token count
nor serialized request size. The loop copies the hash and byte count into
provider-owned request values. Section IDs and versions stay in the prompt layer.
Protocol adapters place controls at the end of submitted system text or native
tools. The
[provider contract](../design-docs/api-portability.md) owns per-target eligibility
and protocol controls, independent of this section layout.

## Public Values

`RenderedPrompt` contains `system_prompt`, `prefix_hash`, `prefix_bytes` and
`tool_catalog_hash`. It retains no section vector or per-section hash. The
`CacheSection` type, `RenderedPrompt::sections` and `tool_catalog_bytes` subtotal
are removed. Hosts use `system_prompt` for the submitted stable text and
`RunTurnResult::transcript` for conversation, including the terminal assistant
response. `RenderInputs::conversation_tail` and
`SectionVersions::conversation_tail` are also removed. Stable field lengths,
section IDs, versions and native fingerprints keep their previous encoding.

## Memory Context

Bound MemoryRecall, MemoryRemember and MemoryForget tools are directly available
by default. The memory section is a bounded index of IDs, titles and content
cues. Complete notes enter the dynamic conversation through model-directed reads.
Scores, read times and mutable counters stay out of index text. Index loading
happens once before the loop, so tool iterations reuse the prefix and accepted
writes change the next prompt's index. The preamble defines consultation and
same-turn durable learning triggers; [memory-system](../design-docs/memory-system.md)
owns their semantics and the distinction between guidance and runtime guarantees.

`bench-prompt` exercises pure rendering with full and reduced native catalogues,
and repeated construction versus reuse across eight iterations.
`scripts/check-prompt-preamble.sh` checks the default system preamble for dynamic
inputs; behavioral tests check bytes, tool fingerprints and cache invalidation.

For a new model-visible surface, consult the relevant proven shape in
<https://github.com/Piebald-AI/claude-code-system-prompts>, then record the adopted
contract in its owning design document.
