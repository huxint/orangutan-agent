# QQ live dialogue verification

## Objective

Complete the requested live QQ conversation test with the existing bound bot and
real C++ AgentSession. The current login executable binds credentials only.

## Slice

Add an opt-in single-event evaluation runner, invoked by a local authenticated
Gateway test harness. The runner trusts only a host-owned private input file,
checks the saved scanning user's direct-chat identity, reopens one persistent
session, and uses the existing QQ dispatcher and token/HTTP transport to reply.
Journal intake and answers before effects; refuse unresolved attempts and
suppress delivered duplicates. Credentials and state remain outside the workspace.
This is a bounded test entry point, not a production Gateway deployment host.
No new runtime dependency or general WebSocket abstraction is needed.

## Verification

Build the runner, run existing QQ/bootstrap tests and make ci. Probe live Gateway
READY, then have the user send private messages to verify real replies and
cross-process session continuity. Do not label controlled tests as live evidence.
Use a private temporary Node built-in WebSocket harness for the live experiment.

## Completion

Document the evaluation command and limitations, commit verified changes, and
report observed live results. Preserve private transcripts and failed attempts.
