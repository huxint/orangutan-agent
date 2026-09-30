# Agent Teams

## Objective

Extend bounded background children into a usable leader-coordinated team: named
specialists keep independent conversations, work runs asynchronously through the
existing scheduler, task dependencies wait for reviewed results, and the leader
can request corrections without recreating a specialist. Preserve authority,
bounded resource use and joined cancellation. Optimize verified delivery cost,
not agent count or raw cache-hit percentage.

## Research And Decisions

Reviewed on 2026-09-30:

- [Claude Code agent teams](https://code.claude.com/docs/en/agent-teams): independent
  sessions, shared tasks, dependency admission, named messaging, local mailboxes,
  automatic result delivery and documented resume/shutdown limitations.
- [Qoder Experts Mode technical article](https://qoder.com/zh/blog/experts-mode-tech),
  published 2026-03-19: a coordinating leader, asynchronous specialists, a DAG,
  a leader inbox and a verification/rework loop. Specialists do not communicate
  directly. Its comparative benchmark is vendor-reported, not reproduced here.
- [Extracted Claude Code prompts](https://github.com/Piebald-AI/claude-code-system-prompts/tree/71f578f0b31abf8f3cd1256e88307fcd55b01f32/system-prompts):
  TaskCreate, TaskUpdate, SendMessage and teammate communication. These are
  third-party prompt extractions, not private implementation source.

Use Qoder's hub topology by default. The existing parent session is the leader;
there is no additional planner model call or compulsory fixed five-role team.
Use Claude's persistent specialist conversations. Parallelize separable work,
serialize decisions and review. The runtime owns scheduling/state; the model
owns decomposition, interpretation and evidence-based acceptance.

An expert's successful turn produces a report, not an accepted task. Only a
leader update for the current attempt unlocks dependencies. Corrections reuse
the same specialist session. Accepted tasks stay immutable; subsequent changes
are new corrective tasks so previous dependency evidence cannot silently change.

## Complete First Slice

- An opt-in team capability on the existing host-owned `BackgroundTasks` service.
  Its registry, global queue, deadlines, result delivery and shutdown remain the
  only background execution mechanism.
- TeamCreate, TeamTaskCreate, TeamTaskUpdate, TeamGet and TeamStop runtime tools.
  Tools use trusted session ownership, validated values, audit and hooks.
- A bounded roster with explicit stable tool subsets; fresh member session and
  approval identities; parent/child permission intersection; no nested teams.
- Leader-created tasks with dependencies on existing tasks (acyclic by
  construction), one in-flight turn per specialist, bounded correction attempts,
  compare-and-set attempt numbers and explicit review/cancellation states.
- Compact leader completion events through the existing persistence-aware inbox;
  full results available on demand. Team messages remain untrusted evidence.
- Reuse a specialist's conversation and stable native catalogue. Keep task IDs,
  assignments, reports and corrections in dynamic messages. Do not copy the
  leader's conversation, broadcast transcripts or add fake cache version keys.
- Host and model-facing behavioral tests, release gates and lifetime sanitizers.

## Cost And Cache Design

The optimization order is: avoid unnecessary delegation; minimize input context;
reuse useful specialist context; batch completion events; preserve stable prefixes;
then tune routing and concurrency using measured results. Read-only review and
coding may need different tool catalogues: fewer tools and narrower authority can
outweigh the loss of a shared cache prefix. Do not advertise unused tools just to
make hashes match. Keep a member's model/route and catalogue stable across its work.

The parent receives bounded evidence, artifact references and unresolved issues,
not every tool transcript. Independent investigation starts with minimal task
context; corrections carry only the changed requirements and concrete feedback.
Common instructions precede member-specific context. Runtime identifiers, clocks,
task state and inbox contents never mutate the stable system prefix.

Local prefix equality is not a server cache hit. Anthropic currently marks the
stable system/tools prefix; that does not establish history-prefix reuse.
Responses uses a prefix key and provider-managed matching. Cross-provider/model
KV transfer is not available. Cache expiry, concurrent cold starts, provider
thresholds and memory-index changes can defeat reuse. Do not send extra warm-up
calls or wait to warm caches without a measured latency/cost benefit.

Cache-aware evaluation must distinguish uncached input, cache reads, cache writes
and output, with protocol-normalized accounting. Compare single-agent, fresh-child
and persistent-team execution on the same task/model/tools and acceptance checks.
Report verified success, wall time, provider attempts, tokens, cache reads/writes,
estimated cost, repair rounds and leader wakeups, including failures. Missing
usage/prices remain unknown. No percentage saving is claimed by this slice.

## Deferred Scope

Process-restart team recovery with a durable inbox/outbox; peer messaging;
automatic skill extraction; per-role model routing; speculative duplicate work;
worktree/subprocess tooling; provider conversation-cache breakpoints; and a hard
monetary budget with in-flight reservations require separate complete slices.
Existing transcripts remain durable. A process-local task receipt never implies
restart recovery or an exactly-once external effect.

## Risks And Verification

- Session lifetime across turns: retain each specialist's owned policy/session
  through joined work; test cancellation during delayed cleanup under ASan/UBSan.
- Duplicate/stale decisions: require task attempt identity, reject concurrent
  execution of one member, and test retries and late reports.
- False completion: test that successful provider output remains in review and
  downstream work cannot start before explicit acceptance; failed/cancelled
  prerequisites never unlock dependents.
- Cost inflation/context contamination: test independent first contexts, stable
  catalogue/prefix on corrections, targeted dependency evidence and coalesced
  leader delivery. Successful plumbing is not evidence of better model quality.
- Capacity and authority: test global pool sharing, bounded tasks/attempts,
  unknown scopes, selected-tool intersection and no child coordination ports.

Commands: affected `xmake build/run test-tool`, `test-bootstrap`; release
`xmake f -y -m release`, `xmake build -j4`, `xmake test -j4`, `make ci`; debug
sanitizer build/tests for changed lifetime paths. No new dependency or library;
keep parsing, pure transitions and async execution in separate translation units.

## Progress

- [x] Read current runtime contracts and primary references.
- [x] Scope topology, ownership, review, cost and cache decisions.
- [ ] Implement the complete native-tool/runtime slice.
- [ ] Verify public behavior, cancellation and release gates.
- [ ] Move lasting contracts to design documents and remove this plan.
