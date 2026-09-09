# Development And Build Boundaries

## Objective

Reduce the development surface to working commands and give compiler selection,
project flags and target composition separate owners. Ordinary release and debug
builds must apply their documented options to libraries, tests and benchmarks.

## Current Contracts

- [STATUS](../../STATUS.md) recommends removing unused development scaffolding
  and fixing toolchain activation as separate complete slices.
- [BUILD_SYSTEM](../../BUILD_SYSTEM.md) owns supported commands and options.
  The default target selection currently bypasses the custom toolchain's flags;
  explicit selection lacks an assembler for dependency builds.
- [Critical rules](../../rules/critical-rules.md) require C++26, narrow public
  headers and truthful enforcement claims. Several advertised checks only print
  success. Package documentation and bucket parity already have real checks.
- [Module boundaries](../../design-docs/module-boundaries.md) require concrete
  callers and explicit ownership. Build scaffolding follows the same rule.
- Persistence formats, stored user data, runtime APIs and dependency versions
  remain outside this development/build slice.

## Implementation Slices

1. Remove the nine success-only scripts, obsolete template initialization and
   unsupported benchmark comparison entry points, the empty build-check include
   and the unused module option. Retain implemented checks and benchmark targets;
   correct their comments, routing and owning documentation.
2. Separate compiler discovery from project compilation/link policy. Select the
   supported compiler at the build root, provide complete toolsets for package
   builds and apply one shared policy to runtime, test and benchmark targets.
   Release LTO and debug ASan/UBSan must work without manual flag overrides;
   hardening and analyzer options must reach the compiler when requested.

## Risks And Limits

- Removing an apparent check could remove a real gate: trace references and
  implementation first, preserve real checks and document review-only rules.
- Activating flags can expose build or runtime defects: inspect actual commands,
  run the existing release and sanitizer suites and resolve affected failures.
- Project flags must not instrument third-party package builds implicitly.
  Keep dependency tool selection separate from project target policy.
- Reference-hardware compile budgets and hosted CI evidence remain separate
  gates. Do not change thresholds or claim certification from local builds.

## Verification

- Run `make help`, the retained repository checks and `make ci`.
- Configure with `xmake f -y -m release`, build with `xmake build -j4` and run
  `xmake test -j4` after each complete slice.
- Inspect compiler/linker arguments for release LTO, disabled LTO, debug
  sanitizers, disabled sanitizers, hardening and analyzer selection. Cover a
  runtime library, test binary and benchmark through existing targets.
- Configure `xmake f -y -m debug --sanitizers=y`, build and run the complete
  sanitizer suite using ordinary targets. Verify package compiler/assembler
  selection without changing pinned dependency versions.
- Restore the normal release configuration, update the build/CI/rule owners and
  handoff, remove resolved debt and delete this plan on completion.

## Progress

- [x] Read the handoff, inspect script callers and identify build flag ownership.
- [ ] Complete and commit development scaffolding cleanup.
- [ ] Complete and verify build policy separation.
- [ ] Update owning contracts, remove resolved debt and commit the final slice.
