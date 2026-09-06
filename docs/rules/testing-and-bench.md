# Tests And Benchmarks

Every library has a `tests/<lib>` Catch2 bucket and a `bench/<lib>` nanobench
bucket. Xmake names them `test-<lib>` and `bench-<lib>`.

Test public behavior: successful results, errors, authorization, limits,
state transitions, cancellation and storage integrity. Use controlled providers
and transports while retaining the meaningful runtime logic. Tests of deleted
application behavior leave with their callers; retain shared-core regressions.

Async tests use a real executor, explicit synchronization and a hard timeout.
Assign awaited results to locals before Catch assertions. Keep independent
scenarios separate. New or changed tests must fail on the defect they guard;
perform deliberate mutations in an isolated copy.

Run affected build/tests during a slice and `make ci` before a commit. Broad
runtime refactors also run the complete release suite. Debug ASan/UBSan checks
are required for lifetime work. [CICD](../CICD.md) owns hosted gate definitions.

Bench when a meaningful design or performance choice needs evidence. Compare
reasonable alternatives under the same workload; prefer the simpler solution
when measurements do not justify complexity. Report compiler, mode and hardware
limits. [compile-budget](compile-budget.md) owns compile-time thresholds.

```sh
xmake build test-tool
xmake run test-tool
xmake build bench-tool
xmake run bench-tool
```
