# Tool Execution

`oran-tool` owns tool definitions, JSON validation, authorization and handlers.
A registered capability is an advertised requirement, never a grant. All effects
from model tool calls pass through `Registry::dispatch`.

## Dispatch

1. Resolve the registered definition. Registration validates its schema and
   preparation callback.
2. Apply the blocking `tool_before` hook once. Veto stops the operation;
   rewritten input becomes the input for every following stage.
3. Prepare the call once from final input. A preparer validates arguments and
   returns an owned executor with optional path intent; it performs no IO.
   Invalid filesystem arguments return `invalid_argument`, audit a denial with
   `reason=invalid_tool_input` and finish error/completion observations without
   waiting for a path, resolving authority or spending approval.
4. When `path_locks` is supplied, acquire shared read or exclusive mutation access
   using the declared capabilities and the prepared path's lexical workspace key.
   Vetoed calls never wait for path resources. Cancelled waits finish observations
   without resolving authority, consuming approval or running an executor.
5. Resolve prepared filesystem intent into pinned authority after acquiring the
   lock, so a queued call observes the preceding operation's completed state.
   A displayed path or an earlier path check cannot authorize a later effect.
6. Evaluate rules for the concrete tool, input, declared capabilities and caller.
   Denial stops execution. An ask decision requires a valid approval.
7. Await the durable decision before invoking the prepared executor at most once.
   Storage failure or cancellation stops execution. Enforce output limits, await
   audit metadata enrichment and publish completion/error observations before
   releasing the lock.

`Registry::add_prepared` registers this pure preparation boundary. Each filesystem
built-in owns its typed arguments and projects path intent from them. Dispatch
retains the prepared value through execution and cleanup. Approval and hooks use
the exact final input bytes, without JSON reserialization.

`Registry::add` adapts ordinary handlers into the same dispatch path. It preserves
the registered handler's state and defers concrete argument validation to that
handler. Filesystem-capable ordinary handlers with a string `path` receive
capability-based exclusion and own their authority handling. Names never select
built-in parsing or authority; a custom prepared tool can declare a target from
its own argument shape.

`DispatchContext` carries identity, rules, audit, workspace, approval and injected
memory services. A tool receives these dependencies explicitly. Application
code supplies bindings once instead of registering alternate dispatch paths.
`register_builtins` installs filesystem tools and catalogue discovery;
`register_memory_tools` adds the three memory tools when the host supplies their
services. `register_agent_run` adds a configured-name child runner supplied by
the host. A session advertises tools available through its bindings.

## Scheduler

`agent::ToolScheduler` provides bounded parallel calls, ordered results and
per-call timeouts. It owns an opaque `tool::PathLocks` resource and binds it to
each dispatch; it does not parse input or classify filesystem capabilities.
Batches and sessions using the same scheduler share exclusion on its coordinating
strand. Direct registry callers can supply a shared resource on their strand.
The owner retains it until all borrowing dispatches finish.

Read-only calls may share a path lock; mutations exclude other accesses to the
same workspace lock key. Keys normalize final input lexically against configured
roots without filesystem access; they are not inode or symlink-alias locks.
Calls without a lockable path still pass through ordinary permission, resolution
and execution. Path intent does not grant any capability.

Per-call timeout starts after the batch semaphore admits the call and includes
hooks, path waiting, authorization and execution. Path waiting counts toward
approval expiry; dispatch advances the caller's clock sample by the monotonic
wait duration before checking the grant.

The lock table retains only live holders, queued waiters and granted handoffs.
The final participant releases the entry synchronously. FIFO handoff reserves a
permit before posting the waiter's completion; a queued writer prevents later
readers from bypassing it. Cancellation returns any reserved permit and advances
the remaining waiters. There is no idle TTL or host-driven cleanup.

Cancellation has a 100 ms batch grace window. A lagging operation is recorded and
may still be alive after the batch returns. An owner must await
`wait_idle(context)` before releasing that context; `wait_idle()` joins all calls.
This lifetime obligation also applies to borrowed audit, workspace and handlers.
Calls retain the scheduler's lock state through their cleanup, including after a
cancelled batch returns.

## Results And Prompt Catalogue

`Output` separates bounded model-visible text from structured JSON, attachments,
usage and error status. Empty, invalid or oversized output is handled by the
owning output contract. Provider/tool loops preserve tool-call IDs so each result
matches the call that produced it.

Catalogue rendering is deterministic from `ToolDef`. Active tools expose full
schemas; deferred tools expose a compact index and are discovered through
`ToolSearch`. Promotion updates the next prompt boundary. The catalogue never
changes permission policy.

## File And Memory Tools

FileRead, FileWrite and FileEdit are the built-in filesystem tools. Reads use read
authority; writes and edits use mutation authority and conflict checks. Write and
edit must not act on a target replaced after authorization. Exact filesystem
semantics live in [io-runtime](io-runtime.md).

Their preparers validate required fields, field types, known options, positive
integer bounds and read-range combinations. Paths must be non-empty and contain
no NUL bytes. Write content limits and invalid edit substitutions are checked
without filesystem access. Unknown fields are rejected as their schemas declare.
File existence, current version, source content and resulting edit size remain
checks after path admission; preparation cannot predict state after a queued writer.
Workspace write intent only carries parent-creation permission. Write modes belong
to the typed write request and the IO operation.

MemoryRecall, MemoryRemember and MemoryForget receive a host-bound scope through
injected handlers. They cannot select another scope in tool JSON. Their JSON shape,
bounds, uniqueness and record-kind values are prepared into typed requests before
path admission, permission evaluation or approval; handlers perform only the
host-bound memory effect. [memory-system](memory-system.md) owns record and prompt
recall semantics.

## Child Agent Tool

`AgentRun` accepts `{"agent":"worker","prompt":"Inspect the change"}` and
requires `spawn_agent`. The registered schema enumerates configured names;
the handler rejects unknown names, extra fields and prompts outside 1–16384
UTF-8 bytes before invoking the host binding. Identity, session, memory scope,
provider route and policy come from the host.

The agent and prompt fields are prepared before authorization; the host binding
receives only the typed request and supplies identity, session, memory scope,
provider route and policy. The result text is the child's completed answer.
Structured output contains
`kind=agent_run`, the configured agent name and its session ID. Output caps,
permissions, approvals, hooks and audit use the ordinary dispatch path. Child
admission exhaustion returns `mailbox_overflowed` with `reason=child_limit` as a
model-visible tool error. Disabled delegation returns `permission_denied`.
[Agent execution](agent-platform.md) owns the host's child-session behavior.

## Capability Vocabulary

The following enum is the stored configuration vocabulary. Some names currently
have no registered handler; they confer no effect by themselves.

```cpp
enum class Capability {
  // file system
  read_file,
  write_file,
  edit_file,
  delete_path,
  list_directory,
  // network
  egress_http,
  egress_websocket,
  // process
  spawn_subprocess,
  signal_subprocess,
  // memory
  read_memory,
  write_memory,
  // orchestration
  spawn_agent,
  send_message_intra_team,
  send_message_inter_team,
  // automation
  schedule_job,
  modify_job,
  run_job_now,
  // skills
  invoke_skill,
  deactivate_skill,
  // misc
  external_mcp,
  runtime_loader,
};
```

[permissions-and-hooks](permissions-and-hooks.md) owns rule precedence and
approval binding. [prompt-design](../rules/prompt-design.md) owns cached sections.
