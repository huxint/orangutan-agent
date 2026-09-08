# Live Debt

[STATUS](../STATUS.md) records the verified core and handoff. Scope each remaining
slice in an execution plan before implementation.

| Area | Remaining obligation | Closure evidence |
| --- | --- | --- |
| Persistence | Back up databases before schema changes and define explicit import mappings. | Backup and import integrity tests. |
| Tool input preparation | Give each filesystem built-in one validated request for path admission and execution; remove duplicated parsing and name-based routing. Concrete argument validation currently occurs in handlers after approval. | Invalid arguments do not consume approval or perform effects; final paths, pinned authority and audit ordering remain consistent. |
| Shell execution | Add an explicitly authorized subprocess boundary with bounded output, cancellation and child authority constraints. | Controlled subprocess and denied-effect tests. |
| Toolchain activation | Apply LTO/sanitizer flags through default target selection; supply the assembler when selecting `oran-gcc` for package builds. Current sanitizer verification uses explicit flags in an isolated copy. | Verbose compile/link commands contain the requested flags and ordinary configure/build succeeds. |
| Hosted quality | Establish hosted C++ job evidence and a clang-tidy/analyzer baseline. | Successful job logs and covered translation units. |
| Compile budget | Calibrate and enforce measurements on reference hardware. | Measured build artifact. |

Persistent user data must remain intact while APIs and composition change.
