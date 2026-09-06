# Agent Execution

An agent turn maps a prepared context to a response and transcript. The current
`agent::Loop` coordinates provider calls and tool batches. `bootstrap::AgentSession`
composes persisted context, scoped memory recall and explicit permission policy.

## Turn Contract

1. Resolve the agent/session identity and permission policy before execution.
2. Load bounded history, retain complete exchanges and recall scoped memory once.
3. Build a deterministic prefix and append the user's message as dynamic context.
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
`RunTurnResult` owns the answer, usage, typed assistant blocks and transcript.
The session coordinator serializes turns using the same session identity.

Persisted history loads at most 128 rows and 512 KiB of encoded content/metadata.
An incomplete leading exchange is removed before model submission. Stored rows
remain intact. Deferred tool promotion affects the next prompt boundary.

[`prepare_conversation`](../../include/oran/agent/conversation.hpp) takes owned
history and prompt values, removes the incomplete leading exchange and returns
`PreparedConversation`. Its `history_size` marks the first message to persist after
success. It requires no services or clock. Provider, tool and storage coordination
stays in the session runner. Prompt recall uses the same authorized dispatch path
as model-requested recall. Do not add mutable application registries or
cross-agent state to the loop.

## Agent Collaboration

The first collaboration layer will own a bounded set of child turns. Each child
gets independent session state and an explicit memory scope. Its effective
permission policy must be no wider than its parent's. Results return as values;
parent cancellation joins all children before releasing their services.

This layer is not implemented. Its first acceptance is one parent and one child,
including a refused child effect and cancellation. Team strategies follow only
when this composition is usable.

[Tools](tool-runtime.md), [memory](memory-system.md),
[permissions](permissions-and-hooks.md), [providers](api-portability.md) and
[async ownership](async-model.md) own their respective contracts.
