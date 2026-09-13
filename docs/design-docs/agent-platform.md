# Agent Execution

An agent turn maps a prepared context to a response and transcript. The current
`agent::Loop` coordinates provider calls and tool batches. `bootstrap::AgentSession`
composes persisted context, scoped memory recall and explicit permission policy.

## Turn Contract

1. Resolve the agent/session identity and permission policy before execution.
2. Load bounded history, retain complete exchanges and load the scoped memory index.
3. Select native tools and render one deterministic system prefix for the turn.
4. Request a provider response. Tool-use responses dispatch through one scheduler
   and registry; append their ordered results and request the next response.
5. Return terminal text and typed content. Persist only the successful transcript
   suffix. Provider/tool/cancellation failures are explicit errors.

The iteration cap is 16 by default; the session supplies a 4096-token completion
limit unless explicitly overridden. Tool concurrency and per-call timeout are
bounded independently. Model-repairable tool errors re-enter the model as error
results; infrastructure failures terminate the turn. Trace rows correlate the
turn with its tool audits without storing raw prompt bodies.

## Context And State

`RunTurnInputs` contains borrowed prompt/context views and explicit service
references. They remain valid until the turn and its tool work finish.
`RunTurnResult` owns the answer, usage, typed assistant blocks, stable
`rendered_prompt` and transcript. `rendered_prompt.system_prompt` is the system
text used by every iteration; conversation remains in the typed transcript.
The loop forwards unfiltered prefix identity to the provider, whose protocol
boundary applies the selected route's cache policy.
The session coordinator serializes turns using the same session identity.

Persisted history loads at most 128 rows and 512 KiB of encoded content/metadata.
An incomplete leading exchange is removed before model submission. Stored rows
remain intact.

`RunTurnInputs` supplies an available tool catalogue and optional active names.
The loop selects and owns a sorted native catalogue once, before provider
execution. The renderer consumes that value and copies stable caller text before
the first provider request. Every iteration reuses the owned prefix and native
declarations. Provider or tool callbacks cannot change that turn's prefix;
the next turn observes host edits. Tool outputs change the conversation, never
selection or stable text. An absent list exposes all available tools, an empty
list exposes none, and unknown names fail explicitly.
The [tool contract](tool-runtime.md) owns selection and dispatch authority.
[Prompt design](../rules/prompt-design.md) owns the reduced rendering values,
joining rules and migration from diagnostic sections.

New trace rows store the native definition fingerprint in `active_catalog_hash`
and zero in the retired `deferred_catalog_hash` column. Existing trace rows and
schema versions remain intact.

[`prepare_conversation`](../../include/oran/agent/conversation.hpp) takes owned
history and prompt values, removes the incomplete leading exchange and returns
`PreparedConversation`. Its `history_size` marks the first message to persist after
success. It requires no services or clock. Provider, tool and storage coordination
stays in the session runner. Automatic memory orientation uses the same
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
scheduler bounds concurrency per batch, so a parent awaiting a child does not
consume that child's tool permits. Parent cancellation propagates through child
provider and tool work; context-specific draining joins cleanup before either
session releases borrowed services. An unrelated session does not extend that
join.

Parent and child rule decisions intersect at every tool dispatch, including
rewritten inputs and automatic recall. The permission contract owns precedence
and approval limits. The self-contained task and returned final-report shape
follows the reference [Agent usage notes](https://github.com/Piebald-AI/claude-code-system-prompts/blob/main/system-prompts/tool-description-agent-simple-usage-notes.md).

[Tools](tool-runtime.md), [memory](memory-system.md),
[permissions](permissions-and-hooks.md), [providers](api-portability.md) and
[async ownership](async-model.md) own their respective contracts.
