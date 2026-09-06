# Roadmap

| Step | Outcome | Acceptance |
| --- | --- | --- |
| Minimal runtime | One executable and one provider/tool/session path; peripheral code and configuration removed. | Build, core integration tests and repository gate pass. |
| Functional boundaries | Explicit session values, pure context/permission functions and injected effects. | State transitions test independently; composition tests exercise real effects. |
| Memory and tool reliability | Scoped recall, persistence, authorization, bounded scheduling and joined cancellation. | A continued session uses saved context; refused actions have no effect. |
| Agent collaboration | Bounded child execution with independent sessions and constrained authority. | A parent receives a child's result and cancels it safely; children cannot widen authority. |

Work in this order. [STATUS.md](STATUS.md) records the current gate;
[the active plan](exec-plans/active/2026-09-06-agent-runtime-core.md) scopes implementation.
