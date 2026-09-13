# Core Surface Reduction

## Goal

Remove orphaned core facilities and their dedicated verification scaffolding.
Keep the public result boundary and runtime cache owners aligned with current
callers, and replace obsolete error-handling guidance with executable patterns.

## Scope And Evidence

- `core::BoundedCache` has no runtime consumer after file-read and rendered-schema
  caches were removed. Delete its header, dedicated tests, benchmark and runner
  registration. Storage's connection-bound statement cache remains its own owner.
- `core::all_ok` has no runtime consumer. Delete its tuple aggregation template,
  unused supporting alias and dedicated tests. Keep the result trait used by
  `io::run_blocking`; remove no live constraint or result behavior.
- Remove unused tuple includes and update the core benchmark inventory, module
  boundary contract and error-handling rule. The latter currently advertises
  eager calls as short-circuiting and references removed helpers.
- Do not change schemas, stored records, configuration, provider behavior,
  authorization, cancellation or resource ownership.

## Constraints And Risks

The architecture and module-boundary contract require current callers for shared
abstractions. Critical rules and the compile budget apply. This slice exceeds
the review-size guideline because whole obsolete files and guidance are removed.

Public API removal requires hosts using these convenience templates to migrate:
use explicit result checks for sequential fallible work and keep caching with
the concrete resource owner. Existing runtime consumers need no replacement.
Removing transitive includes may reveal missing direct includes; build all
runtime libraries and tests. No compile-performance claim is part of acceptance.

## Validation And Completion

- [x] Confirm every symbol's runtime, test, benchmark and documentation consumer.
- [ ] Remove the obsolete APIs and dedicated scaffolding.
- [ ] Update owning contracts and remove obsolete references.
- [ ] Run release library build, all test targets and `bench-core` build/run.
- [ ] Run `make ci`, inspect the diff and commit the complete slice.
- [ ] Delete this plan after its invariants are in the owning documents.

Commands: `xmake f -y -m release`, `xmake build -j4`, `xmake test -j4`,
`xmake build -j4 bench-core`, `xmake run bench-core`, `make ci`.
