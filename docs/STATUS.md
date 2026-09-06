# Current State

Orangutan now builds as a single executable over the provider, tool, permission,
session and memory libraries. The [core plan](exec-plans/active/2026-09-06-agent-runtime-core.md)
owns the next functional refactoring steps.

Peripheral application modules, their configuration and duplicate specifications
have been removed. The executable requires an explicit model configuration and
prompt, preserves completed history, and resumes an explicit session ID.

## Verification

Release build and the complete test suite pass. The application integration test
uses the real HTTP/protocol boundary with a controlled provider and verifies
persisted continuation. Deliberate faults in isolated copies prove the history
and queued-cancellation regressions fail at their intended assertions.

`make ci`, Markdown lint, local document links and formatting checks pass. The
bootstrap assembly benchmark runs. Debug/sanitizer, real-model credentials,
hosted analyzer and reference-hardware measurements remain unverified.

## Next Slice

Separate session context and permission decisions into pure value transformations,
with model, tool and storage effects explicitly composed around them. Keep the
session and authority boundaries suitable for later bounded child-agent execution.
[Live debt](exec-plans/tech-debt-tracker.md) records remaining concrete gaps.
