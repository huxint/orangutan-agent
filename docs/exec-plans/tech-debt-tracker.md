# Live Debt

[STATUS](../STATUS.md) records the verified core and handoff. Scope each remaining
slice in an execution plan before implementation.

| Area | Remaining obligation | Closure evidence |
| --- | --- | --- |
| Deferred-tool state | Review removal of ToolSearch, SessionState promotion handling and PromotionState TTL/LRU wiring: MemoryForget is the only built-in tool still deferred by default. Check real host dependencies before changing this API. | Current host scenarios work with direct native tool declarations; explicit tool selection, child restrictions and dispatch permissions remain correct. |
| Duplicate tool schemas | Active schemas currently appear in both the system-prompt catalogue and provider-native tools. Consolidate tool selection and remove redundant textual schemas for supported native-tool paths, together with the discovery-state reduction. | Provider requests expose each schema once; tool guidance, native calls and cache behavior remain correct. |
| Development scaffolding | Remove unused echo-only script stubs (nine found), the empty xmake/checks.lua include and the unconsumed modules option after checking references. Delete obsolete command advertisements and enforcement claims. | Remaining documented commands perform their stated work; normal builds, tests and make ci pass without placeholder entry points. |
| Persistence | Back up databases before schema changes and define explicit import mappings. | Backup and import integrity tests. |
| Memory behavior | Evaluate spontaneous consultation and durable learning with the deployment model and explicitly supplied credentials. Controlled-provider tests establish the runtime path only. | Unhinted correction and fresh-session cases, one-off instructions, obsolete-note replacement, refusal handling and measured false writes; cases live in the memory contract. |
| Working context | Persist bounded goals, constraints and pending work before old exchanges leave the prompt; add host-supplied provenance for new durable notes after the backup/import contract is available. | Context-pressure and reopened-session cases retain the active task; old rows remain readable and new sources are traceable. |
| Shell execution | Add an explicitly authorized subprocess boundary with bounded output, cancellation and child authority constraints. | Controlled subprocess and denied-effect tests. |
| Toolchain activation | Apply LTO/sanitizer flags through default target selection; supply the assembler when selecting `oran-gcc` for package builds. | Compile/link commands contain the requested flags and ordinary configure/build succeeds. |
| Hosted quality | Establish hosted C++ job evidence and a clang-tidy/analyzer baseline. | Successful job logs and covered translation units. |
| Compile budget | Calibrate and enforce measurements on reference hardware. | Measured build artifact. |

Persistent user data must remain intact while APIs and composition change.
