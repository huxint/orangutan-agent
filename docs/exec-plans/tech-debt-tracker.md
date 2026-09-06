# Live Debt

Implementation order belongs to the
[core plan](active/2026-09-06-agent-runtime-core.md).

| Area | Remaining obligation | Closure evidence |
| --- | --- | --- |
| Persistence | Append a completed turn atomically; back up databases before schema changes and define explicit import mappings. | Transaction rollback, backup and import integrity tests. |
| Storage execution | Move audit and trace SQL off the coordinating strand. | Executor-boundary tests. |
| Cancellation | Verify context-specific tool draining, including a concurrent independent session. | Deterministic lifetime regressions and sanitizers. |
| IO singleflight | Verify cross-executor wake and leader cancellation. | IO regressions. |
| Toolchain activation | Apply LTO/sanitizer flags through default target selection; supply the assembler when selecting `oran-gcc` for package builds. Current sanitizer verification uses explicit flags in an isolated copy. | Verbose compile/link commands contain the requested flags and ordinary configure/build succeeds. |
| Hosted quality | Establish hosted C++ job evidence and a clang-tidy/analyzer baseline. | Successful job logs and covered translation units. |
| Compile budget | Calibrate and enforce measurements on reference hardware. | Measured build artifact. |

Persistent user data must remain intact while APIs and composition change.
