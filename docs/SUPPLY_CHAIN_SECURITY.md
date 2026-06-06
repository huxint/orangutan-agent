# Supply Chain Security

Defaults inherit from the harness-template template and tighten to fit a C++ project
that ships native binaries.

## Default Controls

- **Dependency pinning**: `xmake/packages.lua` pins requested package versions
  and `docs/rules/libraries.md` documents every approved dependency.
- **Dependency review**: available via `actions/dependency-review-action`, but opt-in
  through `ENABLE_DEPENDENCY_REVIEW=true` because GitHub rejects the action before it
  can inspect the PR when repository-level dependency graph / Advanced Security support
  is unavailable or disabled.
- **Vulnerability scanning**: `google/osv-scanner-action` on PRs, scheduled runs, and
  manual dispatch, pinned to an immutable upstream commit. It scans checked-in
  manifests; `xmake-requires.lock` is ignored until CI owns a stable refresh/check flow.
- **SBOM**: `anchore/sbom-action` produces an SPDX SBOM for release artifacts.
- **Provenance**: `actions/attest-build-provenance` generates a signed attestation
  for each release.
- **Pinned actions**: every workflow pins GitHub Actions to immutable commit SHAs.
- **No floating tags**: `scripts/check-action-pinning.sh` fails CI if it spots
  floating tag references.

## Current Workflow Mapping

| Workflow file                     | Role |
| --------------------------------- | ---- |
| `.github/workflows/ci.yml`        | Docs / hygiene / shell-lint / xmake test |
| `.github/workflows/release.yml`   | Release-package job (placeholder until v1 binaries land) |
| `.github/workflows/supply-chain-security.yml` | Dependency review + OSV scan |

## Limits And Assumptions

- Dependency Review needs GitHub's dependency graph support for this repository. Set
  `ENABLE_DEPENDENCY_REVIEW=true` only after the repository's Security Analysis page
  reports that Dependency Review is available; otherwise the workflow skips that job
  instead of reporting a false red status.
- OSV coverage depends on checked-in package declarations and system-package
  manifests; for sources we vendor directly (rare), the manifest is encoded in
  `docs/generated/vendored-deps.json`.
- SBOM quality depends on `xmake/packages.lua` listing canonical versions and
  source URLs.
- Provenance is meaningful once `scripts/release-package.sh` reflects the real
  binary build.
- **OpenSSF Scorecard** intentionally disabled until branch protection, release
  history, and SAST posture exist. Add back once the rules are real.

## C++ Specific Notes

- **Static binaries**: when we ship a fully static binary (musl-clang stretch), the
  SBOM must list every library statically linked.
- **Bundled C dependencies** (libcurl + OpenSSL + sqlite3 + libsodium): we prefer
  system packages on production hosts; requested `xmake` source-build versions
  stay in `xmake/packages.lua` plus `docs/rules/libraries.md`.
- **Optional native extensions**: `sqlite-vec` is resolved only when
  `--vector_memory=y` is configured. Default builds do not download or link it;
  gated builds must keep the package pin and license entry in sync with
  `docs/rules/libraries.md`.
- **Random hashing libraries**: `rapidhash` is a small inline lib; `simdutf` is a
  larger native code dep — both are listed in the SBOM.

## Reviewer Checklist For New Packages

- Is the package in `docs/rules/libraries.md`?
- Is the requested version pinned in `xmake/packages.lua` and documented in
  `docs/rules/libraries.md`?
- Is the source URL canonical (upstream repo, not a mirror unless required)?
- License compatibility verified?
- Compile-cost estimate recorded?
- For optional packages, gated behind an `xmake/options.lua` flag?

## What To Do When The Project Becomes Real

- Add ecosystem-specific lockfiles only when CI owns their refresh/check flow.
- Make the release build deterministic and produce explicit versioned artifacts.
- Gate production deployment on provenance verification when possible.
- Consider verifying attestations in the deployment environment (cluster admission,
  endpoint protection, etc.).

## Related Reading

- [`SECURITY.md`](SECURITY.md)
- [`CICD.md`](CICD.md)
- [`rules/libraries.md`](rules/libraries.md)
