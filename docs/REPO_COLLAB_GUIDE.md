# Collaboration

Start with [STATUS](STATUS.md), [ROADMAP](ROADMAP.md) and the owning architecture
contract. Build one complete slice at a time. Large or risky changes need an
[execution plan](PLANS_GUIDE.md).

Use pure functions for policy and transformations, explicit values for state,
and injected boundaries for effects. Introduce an owning class only when a
resource or cache needs a lifetime. Delete replaced code, unused configuration,
redundant fixtures and stale documents alongside the change.

Keep one logical change per commit. Use an imperative Conventional Commit subject
under 70 characters. Run affected build/tests and `make ci`; record concrete
remaining gates without claiming they passed. User data must survive refactors.

Current contracts have one owner. Update that document when behavior changes;
Git stores history. [docs-in-sync](rules/docs-in-sync.md) and
[testing-and-bench](rules/testing-and-bench.md) define the gates.
