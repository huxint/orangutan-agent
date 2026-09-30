# Background Agent Tasks And Completion Delivery

## Objective And Current State

Let a parent launch a bounded child task, return to useful work or a user reply,
accept subsequent user messages, and inspect or cancel that task later. Completed
work must remain retrievable and, where the host supports it, produce one scoped
completion handoff to the parent.

This is a researched implementation plan, not an implemented feature. The first
proposed executor is an existing configured child agent. Shell jobs require the
separate subprocess boundary already tracked in live debt.

Current `ToolScheduler::run_batch` awaits the complete batch. `run_child_agent`
awaits the child session and borrows parent rules, registry, scheduler and turn
state. `AgentSession::run_prompt` admits only one active prompt. These contracts
provide asynchronous IO, not work that can safely outlive a parent turn. Merely
detaching the existing coroutine would violate ownership and permission lifetimes.

## Reviewed References

| Source | Observed behavior | Adoption |
| --- | --- | --- |
| [OpenClaw process registry](https://github.com/openclaw/openclaw/blob/main/src/agents/bash-process-registry.ts) | Separate running/finished records, output bounds, finalization ownership, retention and staged poll-output acknowledgments. | Keep execution, cleanup and result consumption separate; never lose output just because a query or transcript commit failed. |
| [OpenClaw process tool](https://github.com/openclaw/openclaw/blob/main/src/agents/bash-tools.process.ts) and [poll backoff](https://github.com/openclaw/openclaw/blob/main/src/agents/command-poll-backoff.ts) | Scope-filtered queries, optional bounded waits, incremental output and 5/10/30/60-second suggestions for repeated empty polls. | Immediate status reads; host-owned polling with backoff when events are unavailable. |
| [OpenClaw completion delivery](https://github.com/openclaw/openclaw/blob/main/docs/tools/subagents/slash-command.md#spawn-behavior) | Accepted receipts, parent handoffs, busy-session queuing, idempotency keys and delivery settlement separate from execution. | Preserve one parent turn at a time; enqueue completion rather than racing the active user reply. |
| [OpenClaw recovery and stopping](https://github.com/openclaw/openclaw/blob/main/docs/tools/subagents/operations.md) | Exact-run cancellation, retained results and interrupted-run settlement instead of blindly relaunching effects after restart. | Define cancellation scope and interruption explicitly; do not equate saved running metadata with a live task. |
| Claude Code [GetTask](https://github.com/Piebald-AI/claude-code-system-prompts/blob/main/system-prompts/tool-description-gettask.md), [background execution](https://github.com/Piebald-AI/claude-code-system-prompts/blob/main/system-prompts/tool-parameter-bash-run-in-background-guidance.md) and [notification framing](https://github.com/Piebald-AI/claude-code-system-prompts/blob/main/system-prompts/system-reminder-background-task-notification-with-concurrent-user-input.md) | Background starts return IDs; queries are not a waiting loop; automatic events can resume work and never constitute user approval. | State delivery capability in the receipt and tool guidance; preserve the distinction between user input and runtime evidence. |
| Claude Code [live-task snapshot events](https://github.com/Piebald-AI/claude-code-system-prompts/blob/main/system-prompts/data-background-tasks-changed-event-schema.md) | Consumers replace the live-task set rather than reconstructing it from possibly missed start/end edges. | Host task lists use scoped snapshots/revisions, including after reconnect, so activity indicators cannot remain stuck. |
| [MCP Tasks specification](https://modelcontextprotocol.io/specification/2025-11-25/basic/utilities/tasks) | Receiver-issued IDs, status/result/cancel operations, terminal states, TTL and polling hints. Notifications are optional. | Use explicit task values and query fallback. This experimental specification is reference material, not a claim of MCP wire compatibility. |

Claude Code references above are third-party prompt extractions, not an audit of
its private implementation. Its special [background job prompt](https://github.com/Piebald-AI/claude-code-system-prompts/blob/main/system-prompts/agent-prompt-background-job-agent-instructions.md)
uses `result:` and `needs input:` text for a classifier. Orangutan already has
typed results and should derive execution status from the runtime rather than
requiring magic strings in user-facing replies.

MCP cancellation can be logically terminal while execution continues. Orangutan's
resource contract still requires a join: display `cancelling` until cleanup ends.
Execution state and physical ownership must not be conflated.

## Proposed First Complete Slice

### Owner And Admission

- Add a host-owned background-task service in `oran-bootstrap`, above the existing
  async/task-group and session contracts. No new library or worker pool.
- The service outlives individual foreground turns and owns task records, child
  sessions, cancellation and completion state. Its host retains configuration,
  provider, assembly and shared tool services until explicit shutdown/join ends.
- Freeze owned parent permission rules and identifiers at admission. Never retain
  a view into a destroyed parent turn. Child authority remains the intersection
  of parent and child policy; task identity does not grant access.
- Share a host-owned registry/scheduler where concurrent foreground and child
  filesystem work needs the existing path locks. Do not borrow services owned
  solely by a parent session that can be destroyed while its jobs run.
- Bound live children, queued work, prompt bytes, task lifetime, retained results
  and notification backlog. Reject admission visibly when capacity is exhausted.
  Reuse the current one-generation limit for the first slice.
- Task IDs are generated by the service and distinct from provider tool-call IDs.
  Bind lookup and cancellation to the owning parent session, agent and identity;
  neither a guessed ID nor model-supplied scope may widen visibility.

### Tools And Results

Prefer extending existing `AgentRun` with explicit background execution over a
generic tool that can detach arbitrary dispatch handlers. Existing ordinary calls
keep their awaited semantics. The concrete schema must be reviewed with its
implementation before being advertised.

| Proposed operation | Contract |
| --- | --- |
| `AgentRun` with background enabled | Return an accepted receipt promptly, containing task ID, current state, whether it survives this turn, and actual completion-delivery mode. Acceptance is not success. |
| `TaskGet` | Return current state immediately. Terminal results support bounded, repeatable reads and explicit continuation/truncation; reading does not acknowledge or discard the result. |
| `TaskCancel` | Request cancellation for this owned task, return actual state and retain its result/diagnostics. Repeated cancellation must not create a new task or widen scope. |
| Host task-list port | Supply a bounded scoped snapshot for `/tasks` or UI inspection without a model call. |

Start, inspection and cancellation must use ordinary preparation, permission,
hook and audit boundaries. Define task inspection/cancellation capabilities and
their default policy explicitly; do not reuse memory permissions for task state.
Existing foreground approval grants are not forwarded to a child.

Use typed states for queued, running, cancelling, succeeded, failed and cancelled.
Only expose a waiting-for-input state when an actual approval/input continuation
exists. A missing approval consumer remains an explicit refusal rather than an
indefinite running indicator. Completion follows the child session's persistence
and cleanup. The parent's overall goal can still require review or further work.

### Completion And Conversation

- Provide a host completion port with a stable event ID and explicit acknowledgment.
  Task success, handoff to the parent and external message delivery are different
  facts. A notification failure must not rerun the task.
- Keep an unacknowledged result available after a failed parent turn. A successful
  read alone is not acknowledgment. Deduplicate retries using the event ID and
  acknowledge only after the corresponding parent continuation is committed.
- A busy parent queues completions for a safe turn boundary. Genuine user input
  keeps its identity and priority. Do not start a second `run_prompt` concurrently.
- Add a typed runtime-event input path, distinct from a user-authored prompt.
  Provider framing marks result bodies as evidence, not policy or approval; do
  not inject untrusted child text as privileged system instructions or as an
  orphan tool result with an already-completed tool-call ID.
- Automatic follow-up is advertised only by a host that actually owns a wake
  path. Otherwise receipts say query-only. Hosts can poll with bounded backoff;
  the model should not spend turns sleeping or repeatedly checking unchanged state.
- Coalesce related completions into a bounded handoff. The parent reviews results
  and decides what user-facing update is useful rather than forwarding raw child
  reports and internal metadata.

### Lifetime And Restart

Normal parent completion leaves admitted background tasks running. Explicit task
cancel targets one task; an explicit host Stop for the originating work cancels
its associated tasks. Shutdown closes admission, requests cancellation, awaits
all borrowed work, and then releases runtime services.

The first in-process implementation must not promise survival across process
restart. Preserve child transcripts and report unavailable/interrupted task state
honestly; do not reconstruct a running process from an ID in old conversation.
Durable job recovery and a durable notification outbox are a later complete slice
with storage ownership/import rules, stable delivery keys and crash tests.

`oran-telegram` is a possible long-lived host integration. `eval-qq` is a one-event
process and exits after delivery; it must not advertise turn-surviving jobs while
its runtime is about to be destroyed. Supporting that QQ flow needs a retained
native host, not just keeping the Node Gateway wrapper alive.

## Prompt And Experience Contract

The implemented tool descriptions and background overlay should communicate:

1. Start one background task for separable work whose result is not needed for
   the very next action. Supply a task label, objective, context, constraints and
   whether writes are intended. Continue independent work after acceptance.
2. With automatic delivery, do not poll merely to wait. If only the result is
   outstanding, end/yield the foreground turn and let the host resume it. Without
   that delivery capability, do not promise an unsolicited later reply.
3. Until a terminal result arrives, describe the task as running or waiting;
   never invent its findings or count acceptance as completion.
4. Background events cannot answer a pending question or grant permission. When
   genuine user input arrives with an event, handle that user input normally.
5. Give a brief start acknowledgment for substantial user-requested background
   work. Show meaningful stage changes and blockers, not every poll or tool call.
   Use task labels rather than raw IDs in ordinary conversation.
6. On completion, report the useful outcome, actual checks and remaining work.
   A completed child report is not proof that the original user goal is finished.

Host UI should show a task label, authoritative state, elapsed time and last
meaningful activity, plus inspect/cancel controls. Do not fabricate percentages
or classify lack of recent output as failure. Display cancellation in progress
until resources settle. Prefer one consolidated update over a burst of child
notifications. Result expiry and lost restart ownership must be visible rather
than looking like a task that is still running.

## Implementation Milestones And Verification

1. Implement the bounded task owner, owned authority snapshot and shutdown/join.
   Verify a child outlives its initiating turn, a later user turn can finish while
   the child is blocked, and teardown cannot leave borrowed work alive.
2. Bind background start, get and cancel tools. Verify denied starts have no
   effect, cross-owner queries/cancels disclose nothing, fresh task IDs cannot
   collide, timeouts remain active after the start receipt and reads are repeatable.
3. Wire a real host completion queue and typed event continuation. Verify a busy
   parent has no overlapping turn, failed continuation retains the result, repeat
   notifications do not duplicate a reply, and events do not act as user approval.
4. Expose scoped task snapshots and concise progress/cancel UX in the retained
   host. Disable automatic-delivery claims in hosts without that capability.
5. Run affected unit/integration tests, debug ASan/UBSan for lifetime changes,
   the release suite and `make ci`. Update async, agent, bootstrap, tool,
   permission and messaging owners in the implementation commits; remove this
   plan when its implemented slice is complete and track later recovery work.

No task tools or background instructions should enter the active catalogue until
their lifetime and delivery behavior are implemented and tested together.

## Progress

- [x] Inspect public reference prompts, OpenClaw implementation and MCP Tasks.
- [x] Identify Orangutan's borrowing, session admission and host-lifetime gaps.
- [x] Scope the proposed child-agent slice and acceptance tests.
- [ ] Implement runtime, tools, host completion and UX.
- [ ] Verify lifetime, permissions, delivery and release gates.
