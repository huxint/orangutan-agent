# Working-context retention evaluation

## Objective

Measure deployed-model retention across repeated compactions and reopened sessions
using the existing runtime, with no new third-party dependencies.

## Complete slice

- Add an opt-in `eval-context` executable outside the runtime libraries.
- Use deterministic synthetic conversation fixtures for goals/constraints,
  superseded decisions, observed completion versus pending work, and large tools.
- Pair compacted runs with full-history baselines, reopen each session, and grade
  explicit structured answers without an additional judge model.
- Require actual checkpoint progress and tool execution where applicable; record
  failures separately from protocol, context-pressure and provider errors.
- Isolate and retain each run in a new private directory, with bounded provider
  attempts and request timeouts. Emit JSON metrics including tokens, configured
  cost estimates, elapsed time, model attribution and checkpoint coverage.
- Support controlled local verification and explicit live config/credentials.
  DeepSeek Flash uses its documented Anthropic-compatible endpoint; credentials
  are supplied only to the evaluation process, never to checked-in fixtures.

## Verification and completion

Test grading negatives, scenario construction, resource bounds, and the full
controlled runner including repeated compaction and reopening. Run the executable
against the user-specified DeepSeek model, inspect the report without treating a
small synthetic sample as general task success evidence. Run affected tests,
`make ci`, update build/testing owners, remove this completed plan, and commit.

## Progress

- Runtime and DeepSeek protocol contracts inspected; existing adapter is suitable.
- Implementation and verification pending.
