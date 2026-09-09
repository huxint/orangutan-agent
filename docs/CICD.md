# CI

`.github/workflows/ci.yml` defines two independent gates:

- Repository checks run `make ci`'s script and Markdown lint. The script checks
  documentation, dependencies, public includes, prompt text, action pins,
  repository hygiene and shell syntax. Shellcheck is advisory when available.
- C++ jobs configure, build and test release and debug (requesting ASan/UBSan)
  in a pinned GCC 16 container using xmake 3.1.1. Current flag activation limits
  are documented in [BUILD_SYSTEM](BUILD_SYSTEM.md).

Actions and container images are pinned. Provider tests use controlled fixtures;
credentials are not required for the hosted suite. A workflow definition does
not prove a hosted run passed; [STATUS](STATUS.md) records available evidence.

Authors run the affected builds/tests and `make ci` before committing. Full
runtime refactors run all release tests. Analyzer, clang-tidy and reference
compile-budget enforcement remain separate tracked gates in
[live debt](exec-plans/tech-debt-tracker.md).
