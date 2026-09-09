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

Long-term memory tools borrow one scoped FTS5 backend. Sessions load a bounded
memory index by default; exact-ID reads and topic search supply full notes through
the tool loop. Browsing leaves read timestamps unchanged, and corrections retain
creation/read history. The [memory contract](design-docs/memory-system.md) owns
these boundaries, output compatibility and preservation of existing tables.
SQLite connections configure their own handles from explicit options.

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

Filesystem tools prepare owned, validated calls from final hook input before
path admission and approval. Path intent and execution use that same request;
registry dispatch no longer selects filesystem behavior by tool name. Invalid
arguments audit a denial without consuming approval. The scheduler retains shared
lock resources without interpreting arguments. Path entries follow live holders
and waiters; the final participant releases the entry synchronously. Shared
session exclusion and context-specific cleanup joins use the same scheduler. The
[tool contract](design-docs/tool-runtime.md) owns ordering and lifetime.

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
integration composes `HttpProviderBackend`, `RuntimeAssembly` and `AgentSession`
to cover provider calls and persisted continuation. Tests also exercise scoped
recall, permission intersection, bounded child sessions and cancellation joins.

Public-boundary regressions cover atomic storage preservation, provider routing,
final-input path admission, pinned authority, approval expiry and audit ordering.
Prepared-call regressions cover invalid arguments before approval, rewritten
requests, declared custom targets and retained ordinary-handler state. Isolated
faults and explicit ASan/UBSan instrumentation verify the affected behavior and
ownership boundaries; generated evidence lives under `build/validation`.

Memory regressions cover unhinted prompt inputs, scoped index discovery, exact
reads, same-ID correction, reopened sessions, budgets and unavailable memory.
Four deliberate faults detect disabled orientation, scope escape, unbounded
index projection and lost creation/read history. Controlled providers supply the
tool decisions; real-model consultation and learning quality remain a separate
gate in the memory contract.

Local verification is not reference-hardware compile/performance certification.
The toolchain and hosted quality gaps remain in [live debt](exec-plans/tech-debt-tracker.md).

## Handoff

The host-bound memory and child tools now use the same prepared-call boundary as
filesystem tools. MemoryRecall, MemoryRemember, MemoryForget, AgentRun and
ToolSearch parse and validate owned typed requests before path admission,
permission evaluation and approval; handlers no longer reparse JSON. Memory
scope, child identity, policy intersection and concrete services remain host
bound.

MemoryRecall and MemoryRemember are active in the default catalogue. The runtime
preamble defines proactive consultation and same-turn durable correction; the
index makes existing knowledge discoverable before a model chooses its next
action. Session configuration controls automatic orientation, and all reads and
writes retain their permission boundary. The next memory work is deployment-model
behavioral evaluation and persisted working context before history truncation.
Compaction and record provenance require the pending backup/import boundary.

The library provider/tool/session loop remains the acceptance boundary:
authorized tool execution, scoped memory recall, persisted continuation and
bounded child collaboration.

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
`tests/io/test_directory_authority.cpp`, `tests/tool/test-path-locks.cpp`,
`tests/tool/test-admission.cpp`, `tests/agent/test_scheduler.cpp`,
`tests/bootstrap/test-child-agents.cpp`,
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
