# Agent Execution

An agent turn maps a prepared context to a response and transcript. The current
`agent::Loop` coordinates provider calls and tool batches. `bootstrap::AgentSession`
composes persisted context, scoped memory recall and explicit permission policy.

## Turn Contract

1. Resolve the agent/session identity and permission policy before execution.
2. Load a checkpoint and forward history pages; prepare bounded context and the scoped memory index.
3. Select native tools and render one deterministic system prefix for the turn.
4. Check the context budget before each provider request. Tool-use responses dispatch through one scheduler
   and registry; append their ordered results and request the next response.
5. Return terminal text and typed content. Persist only the successful transcript
   suffix. Provider/tool/cancellation failures are explicit errors.

The iteration cap is 16 by default; the session supplies a 4096-token completion
limit unless explicitly overridden. Tool concurrency and per-call timeout are
bounded independently. Model-repairable tool errors re-enter the model as error
results; infrastructure failures terminate the turn. Trace rows correlate the
turn with its tool audits without storing raw prompt bodies.

Tool results whose usage marks truncation or dropped structured data carry an
explicit notice in the model-visible text and the returned transcript. A truncated
result must not appear complete merely because its tool call succeeded. These
fixed notices are added after payload caps (at most 167 bytes of framing) so even
a tiny text cap cannot hide them. They preserve the call ID, remaining structured
data and success/error status. Context budgeting includes the complete framed
message before the next provider request.

## Context And State

`RunTurnInputs` contains borrowed prompt/context views and explicit service
references. They remain valid until the turn and its tool work finish.
`RunTurnResult` owns the answer, usage, typed assistant blocks, stable
`rendered_prompt` and transcript. `rendered_prompt.system_prompt` is the system
text used by every iteration; conversation remains in the typed transcript.
The loop forwards unfiltered prefix identity to the provider, whose protocol
boundary applies the selected route's cache policy.
The session coordinator admits one prompt per session on its owning strand.
Overlapping calls return `conflict` before context reads or provider work;
admission remains held through cleanup and persistence, and releases on errors.

The loop calls `provider::execution::run` over its borrowed backend and route.
Execution returns owned attribution alongside a result: profile, reported or
configured model, protocol and fallback selection. Response/error hooks
and terminal traces consume that attribution directly. The loop accumulates
execution-priced usage; it does not search route lists, parse error context or
estimate prices. `RunTurnResult::model_used` is the attributed model string.
The turn state machine and its observation are separate: one per-turn observer
publishes provider hooks and writes the single terminal trace for every exit.
A cancelled exit shields cleanup and records its cancellation phase; a failed
trace write annotates the turn error without replacing it.

The provider view is separate from the authoritative transcript. Persisted history
loads forward from the checkpoint in pages of at most 64 rows/512 KiB. Sequence
gaps and an oversized first row fail explicitly; the runtime never skips data.
Each page feeds a bounded view before the next page is loaded. Within a turn,
original user, assistant and tool messages remain intact for persistence.

`RunTurnInputs` supplies an available tool catalogue and optional active names.
The loop selects and owns a sorted native catalogue once, before provider
execution. The renderer consumes that value and copies stable caller text before
the first provider request. Every iteration reuses the owned prefix and native
declarations. Provider or tool callbacks cannot change that turn's prefix;
the next turn observes host edits. Tool outputs change the conversation, never
selection or stable text. An absent list exposes all available tools, an empty
list exposes none, and unknown names fail explicitly.
The [tool contract](tool-runtime.md) owns selection and dispatch authority.
[Prompt design](../rules/prompt-design.md) owns rendering values, joining rules
and content-derived identity.

New trace rows store the native definition fingerprint in `active_catalog_hash`
and zero in the retired `deferred_catalog_hash` column. Existing trace rows and
schema versions remain intact.

`RunTurnInputs` accepts a checkpoint, fixed history end and borrowed forward
reader. `RunTurnResult::transcript` contains the supplied new conversation and
original generated messages, never synthetic summaries or loaded history. Its
checkpoint is provisional until session persistence succeeds.

## Context Compaction

`ContextOptions` defaults to a 131072-token total budget and an 8192-byte summary.
The host must select a budget that fits every configured route target. Input
counting accepts a host tokenizer; the dependency-free default conservatively
counts UTF-8 bytes plus message/block/tool framing allowances. Each image uses a
16384-token allowance instead of counting base64 as text; this is not a calibrated
vision tokenizer. Image data stays in recent typed context and saved history.
Text-only handoff summaries mark image presence rather than copying encoded
bytes; they rely on surrounding conversation for earlier visual findings.
The default count is an estimate,
not an exact protocol tokenizer. Output and thinking reserves count toward the
budget, reserving the largest thinking allowance across the route. Compaction
starts above 75% and selects an older complete prefix aiming for 50%, retaining
the newest group verbatim. Tool calls and all matching results
stay together, including groups crossing history pages.

A separate tool-free, non-streaming provider request produces six bounded sections:
Objective, Constraints, Decisions, Completed, Pending and References. Source
sequence labels support artifact/evidence references. The request uses the existing
execution route, retries, lifecycle hooks and usage accounting, with thinking
disabled for summarization. Structural validation checks complete termination,
UTF-8, required nonempty sections, the byte cap and absence of tool calls.

The handoff enters conversation as explicitly derived historical context, not
system instructions or authority. Native tools and the system prefix remain fixed
for the turn. New instructions and evidence supersede the handoff. Compaction does
not write cross-session notes. A soft failure retains the original view and defers
retry until hard pressure; hard failure or an indivisible oversized exchange
returns `reason=context_budget`. Cancellation propagates, and all original stored
rows survive. Provider failures retain their original error kind.

This adopts log/view separation and complete-group cuts from
[OpenHands](https://github.com/OpenHands/software-agent-sdk/tree/main/openhands-sdk/openhands/sdk/context/condenser),
incremental coverage from
[LangMem](https://github.com/langchain-ai/langmem/blob/main/src/langmem/short_term/summarization.py),
and preservation of user corrections, restrictions and unresolved work from the
[reference summary prompt](https://github.com/Piebald-AI/claude-code-system-prompts/blob/main/system-prompts/agent-prompt-conversation-summarization.md).
Repeated summarization can lose meaning; controlled tests prove the runtime path,
while deployment-model evaluation must measure task/constraint retention.

Automatic memory orientation uses the same
authorized dispatch path as model-requested reads. Model-directed exact-ID reads,
topic search and same-turn correction writes use the ordinary tool loop; the
[memory contract](memory-system.md) owns their behavior. Do not add mutable application registries or
cross-agent state to the loop.

## Agent Collaboration

`AgentRun` starts a configured agent with a self-contained task and returns its
completed answer as a tool result. Its `agent` argument selects an entry from
`config.agents`; the host supplies the provider route, workspace, memory scope,
fresh session ID and approval identity. The child owns its conversation and
selected tool context. Each completed child transcript commits separately from
the parent's transcript.

The host admits at most `AgentSessionOptions::max_child_runs` children per parent
prompt, defaulting to four; zero disables delegation. Only one generation is
permitted. A child cannot call `AgentRun`, including through an injected shared
registry. Concurrent calls consume the same prompt-local admission count.

Children reuse the parent's registry, scheduler and coordinating strand. This
shares filesystem path locks while retaining separate dispatch contexts. The
scheduler bounds live dispatch coroutine frames per batch, retaining queued calls
as values until an admitted call fully completes. A parent awaiting a child does
not consume that child's tool permits, including with a one-slot scheduler.
Nonpositive concurrency or timeout bounds fail before dispatch. Exceptions become
results inside the timeout race, so a failing dispatch cannot stall queued work
until its timeout. Unadmitted calls are never reported as cancellation laggards.
Parent cancellation propagates through child provider and tool work; context-specific draining joins cleanup before either
session releases borrowed services. An unrelated session does not extend that
join.

Parent and child rule decisions intersect at every tool dispatch, including
rewritten inputs and automatic recall. The permission contract owns precedence
and approval limits. The self-contained task and returned final-report shape
follows the reference [Agent usage notes](https://github.com/Piebald-AI/claude-code-system-prompts/blob/main/system-prompts/tool-description-agent-simple-usage-notes.md).

[Tools](tool-runtime.md), [memory](memory-system.md),
[permissions](permissions-and-hooks.md), [providers](api-portability.md) and
[async ownership](async-model.md) own their respective contracts.
