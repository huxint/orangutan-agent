# Tool Execution

`oran-tool` owns tool definitions, JSON validation, authorization and handlers.
A registered capability is an advertised requirement, never a grant. All effects
from model tool calls pass through `Registry::dispatch`.

## Dispatch

1. Resolve the registered definition and validate its input.
2. Apply the blocking `tool_before` hook. Veto stops the operation; rewritten
   input must pass validation, path resolution and permission evaluation again.
3. Resolve filesystem inputs into pinned authority handles. A displayed path
   or an earlier path check cannot authorize a later filesystem effect.
4. Evaluate rules for the concrete tool, input, declared capabilities and caller.
   Denial stops execution. An ask decision requires a valid approval.
5. Await the durable decision before invoking the handler. Storage failure or
   cancellation stops execution. Enforce output limits, await any audit metadata
   enrichment and publish completion/error observations.

`DispatchContext` carries identity, rules, audit, workspace, approval and injected
memory services. A tool receives these dependencies explicitly. Application
code supplies bindings once instead of registering alternate dispatch paths.
`register_builtins` installs filesystem tools and catalogue discovery;
`register_memory_tools` adds the three memory tools when the host supplies their
services. `register_agent_run` adds a configured-name child runner supplied by
the host. A session advertises tools available through its bindings.

## Scheduler

`agent::ToolScheduler` provides bounded parallel calls, ordered results, per-call
timeouts and path locks. Read-only calls may share a path lock; mutations exclude
other accesses to the same workspace lock key. Batches and sessions using the same
scheduler share this exclusion and run on its coordinating strand.

Key derivation currently uses the original call input before `tool_before`.
Aligning the key with finalized rewritten input is the next
[admission slice](../exec-plans/tech-debt-tracker.md).

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

MemoryRecall, MemoryRemember and MemoryForget receive a host-bound scope through
injected handlers. They cannot select another scope in tool JSON. [memory-system](memory-system.md)
owns record and prompt recall semantics.

## Child Agent Tool

`AgentRun` accepts `{"agent":"worker","prompt":"Inspect the change"}` and
requires `spawn_agent`. The registered schema enumerates configured names;
the handler rejects unknown names, extra fields and prompts outside 1–16384
UTF-8 bytes before invoking the host binding. Identity, session, memory scope,
provider route and policy come from the host.

The result text is the child's completed answer. Structured output contains
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
