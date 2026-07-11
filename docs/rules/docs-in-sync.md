# Current Contracts Stay In Sync

Documentation is a map of the runtime as it exists now, not a second version-control
system. Git records what changed; current-contract docs explain what callers,
operators, and maintainers may rely on.

## Update A Doc When Its Claim Changes

| Change | Contract owner |
| --- | --- |
| Library boundary, dependency direction, binary, or repository layout | `docs/ARCHITECTURE.md` and the relevant design doc |
| Public API or lifecycle invariant | the relevant `docs/design-docs/` document; product behavior also updates its spec |
| Configuration field, environment variable, CLI command, or operator workflow | example config plus the owning design/spec/operator doc |
| Build target, option, dependency, toolchain, CI, or compile budget | `docs/BUILD_SYSTEM.md`, `docs/CICD.md`, and the applicable rule |
| Permission, hook, prompt, channel, provider, memory, or tool contract | the owning design doc/spec listed in `AGENTS.md` |
| Binding engineering convention | the owning `docs/rules/` file and rule index |
| Multi-session work state or unresolved finding | the active execution plan or `docs/exec-plans/tech-debt-tracker.md` |

Internal refactors, test additions, and bug fixes whose documented contract already
describes the correct behavior need no ceremonial doc edit. Do not add histories,
manually copied test/assertion counts, release ledgers, or completed-plan archives.

## Quality Bar

- Describe observable behavior and invariants, not implementation narration.
- Prefer one owning document over repeating the same statement in several indexes.
- Examples, command names, paths, config shapes, and public symbols must exist.
- Active plans may describe a future state, but must clearly distinguish it from the
  shipped contract.
- If code and a current contract disagree, fix them together or explicitly change the
  contract before relying on the new behavior.

## Enforcement

`make ci` runs structural checks that can be made reliable without heuristics:

- required current-contract files exist;
- rule/design/spec indexes cover their owned files;
- documented scripts, Make targets, libraries, and dependency versions match the repo;
- every library has its required test and benchmark bucket;
- the runtime prompt preamble retains its cache-safety invariants.

Semantic parity for public headers, config schemas, hook events, and capability enums
should become generated checks where practical. Until then, review the affected owner
directly. CI must not require touching an unrelated timestamp, ledger, or narrative
file merely to prove that work occurred.

## Historical Material

Git history is the archive. Once information is absorbed into a current contract or
live debt row, delete redundant review notes, slice histories, release-note ledgers,
and completed execution-plan narratives. Retain external references only when they
still inform a current design decision.

## See Also

- [`critical-rules.md#c13-durable-rationale-belongs-with-the-contract`](critical-rules.md#c13-durable-rationale-belongs-with-the-contract)
- [`../PLANS_GUIDE.md`](../PLANS_GUIDE.md)
- [`../REPO_COLLAB_GUIDE.md`](../REPO_COLLAB_GUIDE.md)
