# Current State

Orangutan runs one executable over the provider, tool, permission, session and
memory libraries. It requires explicit model configuration and a prompt, saves
completed history and resumes an explicit session ID.

Permission evaluation and conversation preparation operate on explicit values.
The session coordinates bounded history, scoped lexical recall, the provider/tool
loop and persistence. Automatic recall uses the same dispatch, permission, hook
and audit path as model-requested memory tools. The
[core plan](exec-plans/active/2026-09-06-agent-runtime-core.md) orders further work.

## Verification

The release build and complete test suite pass. Controlled HTTP integration
verifies the provider/tool loop and persisted continuation. Deliberate faults
prove the capability, context, recall scope, authorization, catalogue and recall
limit tests fail at their intended assertions.

Agent, bootstrap, tool, memory and config tests pass with ASan/UBSan in an isolated
copy using explicit compiler/linker instrumentation. Default toolchain activation
remains [tracked debt](exec-plans/tech-debt-tracker.md).

`make ci`, Markdown lint, local links and formatting checks pass. Agent and
bootstrap benchmarks run. Real-model credentials, hosted analyzer and
reference-hardware compile measurements remain unverified.

## Next Slice

Commit a completed transcript suffix atomically through one storage operation.
Then add bounded child-agent execution with independent sessions and constrained
authority. [Live debt](exec-plans/tech-debt-tracker.md) records concrete gaps.
