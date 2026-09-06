# Agent Runtime Core

## Objective

Build a small, usable agent runtime before adding application surfaces. A turn
must recall scoped memory, call a model, execute authorized tools, return their
results to the model, and preserve the completed conversation. A second agent
must eventually reuse these same contracts with its own state and no wider
authority.

## Architecture

Use a functional core with explicit effects:

- Values describe agent identity, session state, messages, tool calls, decisions,
  and results. State transitions return values instead of changing global state.
- Pure functions validate inputs, evaluate permissions, select context, and
  render prompts. Time and policy are explicit inputs.
- Provider, tool and storage operations are injected effect boundaries. Asio
  coordinates those operations; RAII owns resources and joins child work.
- The composition root creates services once. The agent core has no knowledge
  of a window, chat platform, scheduler, account-link flow or process supervisor.

Permission checks remain at the execution boundary. A tool catalogue advertises
capabilities; it does not grant them. Resolve and validate the concrete operation,
evaluate the caller's policy, request any required approval, then execute and
audit. Rewritten input is checked again. No UI, model response or child agent can
grant itself authority.

## Sequence

1. Remove peripheral application implementations, unused configuration and
   duplicate documents. Keep one small executable that drives the real core.
2. Separate session state and context preparation from effectful turn execution.
   Move memory integration out of the composition root's monolithic session.
3. Validate the complete provider → tool → provider loop, denied/approved
   effects, scoped recall, persisted continuation and cancellation ownership.
4. Add bounded parent/child agent execution through the same turn function,
   with independent sessions and the intersection of parent and child authority.
   Establish result delivery and cancellation before adding team strategies.

Application interfaces and scheduled delivery can be implemented after these
contracts hold. Their current speculative implementation is removed from the
active codebase. Git retains committed history; user databases are preserved.

## First Slice

Remove desktop, local IPC, messaging adapters, automation and their bootstrap
bridges. Retain provider protocols, tool dispatch, filesystem authority, session
storage, lexical memory and the existing cancellation infrastructure. The
executable accepts an explicit configuration, workspace, session and prompt.
It exercises the runtime without becoming another application framework.

## Verification

Each implementation commit builds affected targets and passes their tests plus
`make ci`. Keep tests of public behavior; remove fixtures for deleted products.
Use controlled providers and real temporary storage for core integration. Prove
new regressions against a deliberate fault in an isolated copy. Run the complete
release suite after the deletion, then debug/sanitizer checks for ownership work.
Real-model acceptance requires explicitly supplied credentials.

## Progress

- [x] Core-first scope and functional programming direction established.
- [x] Peripheral code, configuration and document removal.
- [x] Minimal executable and core integration gate.
- [ ] Functional session and memory boundaries.
- [ ] Bounded agent collaboration.
