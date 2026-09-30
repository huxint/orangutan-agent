# Team Review And Simplification

## Scope

Review the dynamic team implementation against its lifetime, notification and
cache contracts. Fix confirmed defects with reproductions, then remove redundant
state and avoid copying reports for compact native reads. Keep the existing
tools, bounds, independent conversations and background service architecture.

## Findings To Verify

- TeamStop only suppresses the current run of each task; superseded attempts may
  still trigger a completion after the team has stopped.
- Reused member sessions retain the team-creation parent trace ID rather than
  correlating each assignment/correction with its initiating parent turn.
- Execution state is reconstructed in reads and cancellation code while a second
  member busy flag is maintained separately. Centralize job-to-task transitions
  and derive member admission from task state.
- Native board/single-report reads copy all retained reports before filtering.
  Render directly from the owned board and serialize each output once.

## Verification

First reproduce notification and trace defects with controlled bootstrap tests.
Run team/background/child/session regressions plus affected agent/tool tests.
Lifetime changes require ASan/UBSan. Finish with the release build, complete test
suite, Telegram target and `make ci`; update the team/turn contract and remove
this plan when complete. No new dependency or library is required.
