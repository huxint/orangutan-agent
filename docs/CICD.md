# CI/CD Guide

The repository ships **language-agnostic** CI plumbing on day one. Once C++ code
lands, project-specific verification slots into `scripts/ci.sh` and target-specific
GitHub Actions jobs.

## What Exists By Default

- `.github/workflows/ci.yml` — repository hygiene + docs + shell lint + markdown lint
  today. C++ build/test jobs remain unprovisioned.
- `.github/workflows/release.yml` — placeholder release pipeline; replace once a real
  binary exists.
- `.github/workflows/supply-chain-security.yml` — opt-in dependency review + OSV
  scan. Dependency Review requires GitHub's dependency graph support for this
  repository and only runs when `ENABLE_DEPENDENCY_REVIEW=true`; OSV runs by
  default.

## Design Principle

CI in early scaffolding proves out the delivery plumbing **without pretending to know
the real build command**. As the project grows, the C++ build naturally takes more
of the CI run; the doc/hygiene gates remain.

## Markdown Lint

`ci.yml` runs [`markdownlint-cli2-action`](https://github.com/DavidAnson/markdownlint-cli2-action)
(SHA-pinned) over `**/*.md`, excluding `docs/generated/**`, configured by the
repo-root [`.markdownlint.json`](../.markdownlint.json).

That config keeps markdownlint's defaults **on** but disables the cosmetic /
high-churn rules this agent-authored docs corpus does not follow — notably `MD060`
(table-pipe alignment), `MD004` (list-marker style), and `MD031`/`MD032` (blank-line
spacing). This is a deliberate **relaxation, not a cleanup**: the existing docs were
never made compliant, so those rules are off to keep the gate green, while the rules
the corpus already satisfies stay enabled and keep catching regressions.

Do **not** re-enable a disabled rule without first making every matched `**/*.md`
file pass it — otherwise every push goes red again. Reproduce CI locally with the
pinned linter:

```sh
npx markdownlint-cli2@0.22.0 "**/*.md" "!docs/generated/**"
```

## Recommended Customization Sequence

1. **Keep** `ci.yml` as the always-on repository gate.
2. **Extend** `scripts/ci.sh` with C++ verification:
   - `xmake f -m release`
   - `xmake -j$(nproc)`
   - `xmake test`
   - `scripts/check-compile-budget.sh`
3. **Replace** `scripts/release-package.sh` with real packaging once binaries exist.
4. **Add** environment-specific deployment jobs once a runtime target exists.
5. **Keep** artifact provenance + SBOM generation in place.

## Supply Chain Gates

`supply-chain-security.yml` keeps the PR-time supply-chain checks separate from the
base repository gate. The Dependency Review job is explicitly opt-in through the
repository or organization variable `ENABLE_DEPENDENCY_REVIEW=true`; GitHub otherwise
returns "Dependency review is not supported" on repositories where dependency graph /
Advanced Security support is unavailable or disabled. The OSV scanner remains always-on
for PRs, scheduled runs, and manual dispatch, using a SHA-pinned upstream action.

## CI Matrix

The target C++ matrix (not active today) is:

| Compiler  | Mode    | Modules | LTO  | Sanitizers |
| --------- | ------- | ------- | ---- | ---------- |
| GCC 16.1  | release | yes     | yes  | no         |
| GCC 16.1  | debug   | yes     | no   | asan+ubsan |
| Clang 19  | release | no      | yes  | no         |

Nightly extends with:

- `clang-tidy` full pass.
- Bench suite + comparison.
- Compile-time baseline check.

## Release Workflow Output

The placeholder release pipeline produces:

- `release-manifest.json`
- `repo-metadata.tgz`
- `sbom.spdx.json`
- a GitHub artifact attestation for the packaged artifact

Once binaries exist, this expands to:

- `orangutan-vX.Y.Z-linux-x86_64.tar.gz`
- `orangutan-vX.Y.Z-linux-x86_64.sig`
- `orangutan-vX.Y.Z-debug.tar.gz`
- the same SBOM + attestation.

## Local CI Approximation

```sh
make ci        # docs + hygiene + shell lint
xmake build    # C++ build (once provisioned)
xmake test     # all tests
```

## See Also

- [`SUPPLY_CHAIN_SECURITY.md`](SUPPLY_CHAIN_SECURITY.md)
- [`RELIABILITY.md`](RELIABILITY.md)
- [`BUILD_SYSTEM.md`](BUILD_SYSTEM.md)
- [`rules/workflow.md`](rules/workflow.md)
