# Product Direction

Orangutan provides a reusable agent runtime. The first milestone is a reliable
single-agent turn using real tools and persistent memory. Multi-agent execution
comes next through the same contracts.

Priorities, in order:

1. Complete a model → tool → model interaction with an observable result.
2. Enforce permissions on every effect, including memory access and child work.
3. Preserve scoped context and make recall, failures and cancellation predictable.
4. Keep values and pure transformations separate from effectful execution.
5. Add a capability only when a concrete core use case requires it.

Keep one implementation per boundary. Delete replaced callers, unused settings,
speculative extension layers and their documentation in the same change. Preserve
user data and useful regression coverage. Git owns implementation history.

Application surfaces follow a stable core. The
[runtime plan](exec-plans/active/2026-09-06-agent-runtime-core.md) defines the
current slice and acceptance criteria.
