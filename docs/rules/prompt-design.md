# Runtime Prompt Design

[prompt::render](../../include/oran/prompt/render.hpp) is a synchronous value
transformation over native tool values and stable caller text. It returns owned
system text and cache identity. The [agent loop](../design-docs/agent-platform.md)
owns one rendered prefix per turn.
This rule governs runtime prompts, not development-agent routing instructions.

## System Text

Nonempty text sections join in this order with one newline:

1. System preamble: identity, operating principles and response contract.
2. Optional caller-supplied skill catalogue.
3. Scoped memory index or exact caller framing selected at the prompt boundary.
4. Stable per-agent instructions.

Rendering preserves the supplied bytes, including whitespace and existing
newlines. All-empty input produces an empty string. Conversation enters
`provider::Request::messages` as typed content, including user messages and tool
results. Clocks, request IDs, trace IDs, counters and status narration stay out
of stable text.

## Native Tools And Identity

The loop selects one owned, sorted catalogue through `tool::select_tools`.
Both protocols receive descriptions and schemas through
`provider::Request::tools`. System text contains no duplicate native definitions.
The [tool contract](../design-docs/tool-runtime.md) owns selection and authority.

`RenderedPrompt` holds `system_prompt`, `tool_catalog_hash`, `prefix_hash` and
`prefix_bytes`. The native fingerprint includes ordered names, descriptions and
opaque schema bytes, with field lengths preserving boundaries. Required
capabilities remain local permission metadata.

The prefix fingerprint depends on joined system text and the native fingerprint.
Equivalent submitted text and declarations have the same identity regardless of
how caller text was divided into sections. There are no manual section versions
or section-ID inputs. Text, schema, selection and declaration-order changes
invalidate identity. Conversation and local permission metadata do not.

`prefix_bytes` counts the joined system text, including separator newlines, and
native name/description/schema bytes. It excludes protocol framing; it is neither
a token count nor serialized request size. Derived cache keys are not a versioned
compatibility contract. The loop forwards current values to the provider; the
[provider contract](../design-docs/api-portability.md) owns target eligibility and
wire controls.

## Memory Context

Bound memory tools are directly visible by default. The stable memory section
is a bounded index of IDs, titles and cues. Complete notes enter dynamic
conversation through model-directed reads. Index loading occurs once before the
loop; accepted writes appear in the next prompt's index. Scores, read times and
mutable counters stay out of index text. [Memory](../design-docs/memory-system.md)
owns consultation and same-turn durable learning guidance.

`bench-prompt` compares full/reduced catalogue rendering and repeated construction
versus reuse across eight iterations. Tests cover joined bytes, identity changes,
native declarations and per-turn snapshots. `scripts/check-prompt-preamble.sh`
checks the default preamble for dynamic inputs.

For a new model-visible surface, consult the relevant proven shape in
<https://github.com/Piebald-AI/claude-code-system-prompts>, then record the adopted
contract in its owning design document.
