# `bench/agent`

Agent benchmarks cover session-owned state that will sit on the ReAct loop's
hot path.

| Scenario | Compares |
| --- | --- |
| [`scenarios/prompt_cache_hit_rate.cpp`](scenarios/prompt_cache_hit_rate.cpp) | Stable prompt builds without promotions vs. builds after `ToolSearch` promotes one deferred tool. |
| [`scenarios/scheduler_overhead.cpp`](scenarios/scheduler_overhead.cpp) | Direct `Registry::dispatch` of a no-op tool vs. a single-call `ToolScheduler::run_batch` of the same tool (spec 0012 AC12). |
| [`scenarios/scheduler_audit_fanout.cpp`](scenarios/scheduler_audit_fanout.cpp) | An 8-call batch recording through `NullAuditSink` vs. through `StorageAuditSink` over an in-memory `Pool` (audit-writer fan-out under bounded parallelism). |

The prompt-cache scenario asserts that `RenderedPrompt::prefix_hash` stays
identical across iterations after the promotion snapshot is fixed; timing is
secondary to catching accidental cached-prefix drift.

`scheduler_overhead` closes spec 0012 AC12: a single-call `run_batch` must stay
within spec 0002's per-call dispatch budget. The scheduler's fixed per-batch
cost (channel-as-semaphore permit, ordered-result drain, per-call timeout race)
sits a few microseconds above a no-op direct dispatch — an order of magnitude
under the ≤ 75 µs scheduler allowance (1.5× the ≤ 50 µs single-call ceiling).
Because a no-op `NullAuditSink` dispatch is itself only a couple of
microseconds, the B/A *ratio* looks large while the absolute overhead is tiny;
for a real tool (file I/O, dozens of µs) it is negligible.

`scheduler_audit_fanout` checks the spec 0012 risk that N concurrent `record`
calls starve the audit writer. The batch runs at full `max_parallel_tools`
(the fake tool declares no capabilities, so no per-path lock serialises it) and
the in-memory `Pool` removes disk-fsync latency so the ratio reflects
writer-strand coordination rather than the storage medium.
