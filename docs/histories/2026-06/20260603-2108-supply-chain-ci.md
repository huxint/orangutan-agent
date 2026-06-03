## [2026-06-03 21:08] | Task: Fix supply-chain CI false reds

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `local CLI`
- Linked plan: none

### User Query

> 修复

### Changes Overview

- Areas: GitHub Actions supply-chain workflow and CI documentation.
- Key actions: Dependency Review now runs only when `ENABLE_DEPENDENCY_REVIEW=true`
  is set after GitHub reports the repository supports the feature, and the OSV scanner
  action pin now points at the valid v2.3.8 upstream commit.

### Design Intent

PR #3 proved the repository-local checks and C++ tests were green, but the separate
supply-chain workflow still went red for reasons unrelated to the PR diff: GitHub
reported Dependency Review as unsupported for the repository, and the OSV scanner
action reference pointed at a commit that does not exist upstream. The fix keeps the
  security gates useful without making repository-level platform limits block every
PR: Dependency Review is opt-in once the GitHub repository can support it, and OSV
remains always-on with a valid SHA pin.

### Files Modified

- `.github/workflows/supply-chain-security.yml`
- `docs/CICD.md`
- `docs/SUPPLY_CHAIN_SECURITY.md`
- `docs/QUALITY_SCORE.md`
- `docs/STATUS.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/CICD.md` — documents the private-repository opt-in variable and the split
  between base CI and supply-chain checks.
- `docs/SUPPLY_CHAIN_SECURITY.md` — documents Dependency Review support limits and
  OSV's valid immutable pin.
- `docs/QUALITY_SCORE.md` — refreshes the supply-chain row with the current gate
  behavior.
- `docs/STATUS.md` — moves the latest-history pointer to this CI fix.

### Validation

- Commands run: `make ci`.
- Tests added/changed: none; workflow/documentation fix only.
- Bench impact: none.
- Compile-budget delta: none.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: none.
