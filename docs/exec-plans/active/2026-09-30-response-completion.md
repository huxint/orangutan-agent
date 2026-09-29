# Complete Responses Before Effects

## Objective And Scope

The loop currently accepts max_tokens/cancelled as successful terminal responses
and dispatches tool blocks before checking the stop reason. Require a completed
provider response before tool effects or successful transcript persistence.

Accept end_turn, stop_sequence and tool_use responses through the existing paths.
Reject token exhaustion and upstream error stops with explicit upstream errors;
map cancelled to cancellation. Preserve usage and attribution in terminal traces.
Do not automatically retry a response or execute a syntactically complete call
from an incomplete response. Earlier accepted effects remain governed by their
ordinary audit and persistence contracts; this is not transactional rollback.

## Verification

Test all rejected reasons with and without valid tool blocks, asserting no tool
effects, no extra provider call, and correct terminal traces. Exercise persistent
session failure and subsequent continuation from previously completed history.
Run affected agent/bootstrap targets, the full release suite and make ci. Update
the agent contract, commit, and delete this plan after completion.
