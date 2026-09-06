# Current State

Orangutan runs a C++26 provider/tool/session loop with explicit configuration,
scoped memory and persisted continuation. The runtime core and filesystem-tool
reduction are complete. [Architecture](ARCHITECTURE.md) and the owning design
contracts describe the implementation.

## Delivered Contracts

Completed transcript suffixes serialize before acquiring the storage writer and
commit in one transaction through `Store::append_all` and
`SessionRepository::append_messages`. A later insert or serialization failure
leaves the preceding conversation intact. Existing schemas, message encoding and
stored user data are preserved. Authorized memory-tool effects commit separately.

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

## Verification

The release build, all 14 test targets and `make ci` pass. Controlled HTTP
integration covers the provider/tool loop and persisted continuation. Agent,
bootstrap, tool, memory, permission and prompt tests pass with explicit ASan/UBSan
compiler and linker instrumentation in an isolated debug copy.

Isolated faults fail at their intended assertions for atomic rollback,
serialization failure, message ordering, identity/scope isolation, policy
intersection, approval limits, rewritten input, child admission/depth, cancellation
joins, catalogue selection, cache versions and the filesystem-tool surface.
Restoring the implementations returns the tested cases to green. Tool, prompt,
agent, config and permission benchmarks build and run; these are local runs,
not reference-hardware performance certification.

## Handoff

The completed plan has been absorbed into the owning contracts. Start further
work from [live debt](exec-plans/tech-debt-tracker.md), preserving user databases.
Shell execution is not registered. Its next slice needs an authorized subprocess
boundary, bounded output, cancellation and authority constraints for children.
Audit/trace SQL offloading, IO singleflight, default toolchain activation and
hosted analyzer/compile-budget gates also remain open. Real-model execution still
requires explicitly supplied credentials.

Use the normal release gate from the repository root:

```sh
xmake f -y -m release
xmake build -j4
xmake test -j4
make ci
```

Relevant regressions are in `tests/bootstrap/test-child-agents.cpp`,
`tests/tool/test-agent-run.cpp`, the permission intersection cases, and the
storage/memory tests tagged `[atomic]`. Child lifetime checks include a concurrent
independent session and a tool that delays cancellation cleanup.

Local validation copies, fault scripts and logs live under `build/validation`;
they are generated artifacts, not required source. For a nested isolated copy,
run xmake from inside that copy and pass `-P .` to configure, build and run.
Sanitizer verification requires explicit
`-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all`
compiler flags and `-fsanitize=address,undefined` linker flags. Confirm the actual
commands; `--sanitizers=y` alone does not establish coverage. LeakSanitizer needs
to run outside a ptrace-restricted sandbox. [BUILD_SYSTEM](BUILD_SYSTEM.md) owns
the supported toolchain and current activation limits.
