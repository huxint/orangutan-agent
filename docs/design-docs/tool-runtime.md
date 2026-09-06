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
5. Record the decision, invoke the handler, enforce output limits and publish
   completion/error observations.

`DispatchContext` carries identity, rules, audit, workspace, approval and injected
memory/skill services. A tool receives these dependencies explicitly. Application
code supplies bindings once instead of registering alternate dispatch paths.

## Scheduler

`agent::ToolScheduler` provides bounded parallel calls, ordered results, per-call
timeouts and path locks. Read-only calls may share a path lock; mutations exclude
other accesses to the same canonical path. Lock entries are reaped by idle TTL.

Cancellation has a 100 ms batch grace window. A lagging operation is recorded and
may still be alive after the batch returns. An owner must await
`wait_idle(context)` before releasing that context; `wait_idle()` joins all calls.
This lifetime obligation also applies to borrowed audit, workspace and handlers.

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

FileRead, FileSearch and DirectoryList operate within read authority. FileWrite,
FileEdit and FileDelete use write/mutation authority and conflict checks. Write
and edit must not act on a target replaced after authorization. Exact filesystem
semantics live in [io-runtime](io-runtime.md).

MemoryRecall, MemoryRemember and MemoryForget receive a host-bound scope through
injected handlers. They cannot select another scope in tool JSON. The existing
skill handlers follow the same dispatch path. [memory-system](memory-system.md)
owns record and prompt recall semantics.

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
