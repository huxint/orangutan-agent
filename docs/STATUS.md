# Current State

Orangutan runs a C++26 provider/tool/session loop with explicit configuration,
scoped memory and persisted continuation through library composition. The runtime
core and filesystem-tool reduction are complete. [Architecture](ARCHITECTURE.md)
and the owning design contracts describe the implementation.

## Delivered Contracts

Completed transcript suffixes serialize before acquiring the storage writer and
commit in one transaction through `Store::append_all` and
`SessionRepository::append_messages`. A later insert or serialization failure
leaves the preceding conversation intact. Existing schemas, message encoding and
stored user data are preserved. Authorized memory-tool effects commit separately.

Session memory owns conversation serialization; storage owns atomic writes and
record readback for sessions, audits and traces. Existing session metadata, skill
rows and audit views survive database reopening and subsequent appends. The
[storage contract](design-docs/storage-runtime.md) owns these compatibility bounds.

Audit decisions, metadata enrichment and terminal traces run on explicit worker
executors and return to the coordinating strand. Pool lease completions preserve
the requesting executor. Dispatch awaits the durable decision before tool effects.
Cancelled audit and trace writes finish before borrowed services are released and
return an explicit cancellation result.

Long-term memory tools borrow one scoped FTS5 backend. `longterm::recall` returns
owned hits and prompt framing; the [memory contract](design-docs/memory-system.md)
owns read timestamps, output compatibility and preservation of existing index
tables. SQLite connections configure their own handles from explicit options.

`AgentRun` selects a configured child, assigns a fresh session ID and approval
identity, and uses the child's prompt overlay and promotion state. Parent and child rules
intersect at dispatch, including rewritten input and approval limits. The child
inherits workspace, memory scope, provider route, scheduler and strand. Admission
defaults to four children per prompt and one generation. Cancellation joins the
child's cleanup without waiting for an unrelated session. The
[agent contract](design-docs/agent-platform.md) owns these bounds.

FileRead, FileWrite and FileEdit are the built-in filesystem tools. ToolSearch,
memory tools and AgentRun provide the other runtime extensions. Stored capability
names remain readable for compatibility.

File IO shares one descriptor-based read implementation and the pinned file
mutation boundary. Each read owns its resources and observes current file bytes;
the [IO contract](design-docs/io-runtime.md) owns ranges, fingerprints and
cancellation. `PrivateDirectory` provides private state ownership for hosts.

Provider construction maps configuration to owned profile values, then builds one
system over an injected transport. Complete route validation precedes credential
lookup. Endpoint credentials stay inside the system; dispatch checks the selected
profile, model and protocol before sending. The
[provider contract](design-docs/api-portability.md) owns this boundary, HTTP/SSE
request lifetimes and delivery of stream callbacks before completion.

Hooks expose the provider, tool, memory and approval events emitted by the
runtime. `EventTraits` defines blocking admission; the
[hook contract](design-docs/permissions-and-hooks.md) owns decisions and payloads.

## Verification

The release library build, all 14 test targets and `make ci` pass. Controlled HTTP
integration covers provider calls and persisted continuation through the library
composition APIs. Core
storage, memory, hook, tool, HTTP and bootstrap ownership cases have passed with
explicit ASan/UBSan compiler and linker instrumentation in an isolated debug copy.

Isolated faults fail at their intended assertions for atomic rollback,
serialization failure, message ordering, identity/scope isolation, policy
intersection, approval limits, rewritten input, child admission/depth, cancellation
joins, catalogue selection, cache versions and the filesystem-tool surface.
IO regressions detect stale content, reopened authority handles and dropped
queued cancellation. Restoring the implementations returns the tested cases to
green. Memory regressions detect missing query validation, scope filtering, read
timestamps, prompt framing, score fields and preserved database content. Hook
regressions reject unintended blocking admission and detect changed payloads or
dropped approval decisions.
Storage preservation regressions detect lost skill, audit and trace rows and a
dropped reporting view. Child persistence checks detect missing or unexpected
session rows; restored implementations pass both sets of checks.
Executor-boundary regressions detect misplaced SQL, missing worker bindings,
swallowed audit errors, unjoined writes and lost cancellation. These cases and
pool lease tests pass with explicit ASan/UBSan instrumentation.
Provider construction regressions detect premature credential lookup, invalid
endpoints, duplicate profiles, credential disclosure and mismatched routes.
Endpoint selection, model-policy projection, host lookup injection and cancelled
stream results have isolated red/green evidence. Provider and bootstrap tests pass
with explicit ASan/UBSan instrumentation, including concurrent profile streams,
joined transport cancellation and HTTP-backed persisted continuation.
IO, HTTP, storage, tool, memory, hook, prompt, agent, config and permission
benchmarks build and run; these are local runs, not reference-hardware performance
certification.

## Handoff

The next core slice is scheduler path-lock ownership. Idle-entry reclamation is
exposed through `reap_idle_locks`, but the session runtime has no caller. Bind
reclamation to the scheduler's actual lifetime and narrow unused controls while
preserving shared path exclusion and context-specific cancellation joins.
The library provider/tool/session loop remains the acceptance boundary:
authorized tool execution, scoped memory recall, persisted continuation and
bounded child collaboration. The controlled HTTP continuation test composes
`HttpProviderBackend`, `RuntimeAssembly` and `AgentSession` directly.

[Live debt](exec-plans/tech-debt-tracker.md) records integration gates and extension
prerequisites. Default toolchain activation and hosted analyzer/compile-budget
gates remain open. Real-model execution requires explicitly supplied credentials.

Use the normal release gate from the repository root:

```sh
xmake f -y -m release
xmake build -j4
xmake test -j4
make ci
```

Relevant regressions are in `tests/io/test_file.cpp`,
`tests/io/test_directory_authority.cpp`, `tests/bootstrap/test-child-agents.cpp`,
`tests/tool/test-agent-run.cpp`, the permission intersection cases, and the
storage/memory tests tagged `[atomic]`. Child lifetime checks include a concurrent
independent session and a tool that delays cancellation cleanup.
Repository reopen checks are tagged `[preservation]` in the storage tests.
Scoped recall and hook gates are covered by `tests/memory/test_longterm.cpp` and
`tests/hook/test_publish_blocking.cpp`; HTTP/SSE coverage lives in `tests/http`.

Local validation copies, fault scripts and logs live under `build/validation`;
they are generated artifacts, not required source. For a nested isolated copy,
run xmake from inside that copy and pass `-P .` to configure, build and run.
Sanitizer verification requires explicit
`-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all`
compiler flags and `-fsanitize=address,undefined` linker flags. Confirm the actual
commands; `--sanitizers=y` alone does not establish coverage. LeakSanitizer needs
to run outside a ptrace-restricted sandbox. [BUILD_SYSTEM](BUILD_SYSTEM.md) owns
the supported toolchain and current activation limits.
