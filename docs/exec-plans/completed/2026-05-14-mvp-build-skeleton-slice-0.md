# MVP Build Skeleton — Slice 0

## Goal

Land the first compiling, testable, benchable cut of Orangutan v2. Slice 0 ships
just enough scaffolding to prove the toolchain, error model, and library-parity rule:
GCC 16.1 + C++26, one library (`oran-core`), one binary (`orangutan`) that prints a
greeting through `std::print`, one tests bucket, one bench bucket. Every other
library listed in [`../../ARCHITECTURE.md`](../../ARCHITECTURE.md) remains shaped only
and is delivered in subsequent slices.

## Scope

- In scope:
  - Repo-root `xmake.lua` and `xmake/{options,toolchain,packages,targets,tests,bench,checks}.lua`.
  - `include/oran/_pch.hpp` (stdlib-only for slice 0; `<fmt/core.h>` and
    `<nlohmann/json_fwd.hpp>` deferred to the slice that introduces a dependent lib).
  - `include/oran/core/{error.hpp,result.hpp}` + `src/oran-core/error.cpp`.
  - `src/main.cpp` — `orangutan` binary using `std::print`, `core::Result` end-to-end.
  - `tests/core/` Catch2 v3 bucket.
  - `bench/core/` nanobench bucket with one A/B comparison.
  - Rule update: standard ratchets from C++23 to **C++26** project-wide; `std::print`
    over iostreams; new `docs/rules/static-analysis.md` documenting `-fanalyzer`.
  - Doc sync: `BUILD_SYSTEM.md`, `code-style.md`, `critical-rules.md`,
    `core-beliefs.md`, `module-and-pch.md`, `compile-budget.md` (no number changes;
    GCC 16.1 baseline still applies), `AGENTS.md` conventions row, `QUALITY_SCORE.md`,
    `releases/feature-release-notes.md`, history entry.
- Out of scope (deferred to future slices):
  - `oran-async`, `oran-storage`, `oran-log`, …  any library other than `oran-core`.
  - asio integration, the agent loop, channels, providers.
  - C++26 modules. Slice 0 stays header + PCH. Migration recipe in
    [`../../rules/module-and-pch.md`](../../rules/module-and-pch.md) remains the plan.
  - Clang secondary toolchain in CI. GCC 16.1 only for slice 0.

## Context

- Relevant docs:
  - [`docs/ARCHITECTURE.md`](../../ARCHITECTURE.md) — library inventory and one-way deps.
  - [`docs/BUILD_SYSTEM.md`](../../BUILD_SYSTEM.md) — xmake shape and toolchain.
  - [`docs/rules/critical-rules.md`](../../rules/critical-rules.md) — non-negotiables.
  - [`docs/rules/compile-budget.md`](../../rules/compile-budget.md) — TU budgets.
  - [`docs/rules/error-handling.md`](../../rules/error-handling.md) — `Result<T>` shape.
  - [`docs/rules/testing-and-bench.md`](../../rules/testing-and-bench.md) — Catch2/nanobench.
  - [`docs/rules/module-and-pch.md`](../../rules/module-and-pch.md) — PCH set.
- Relevant code paths: everything created in this slice — there is no prior C++ code.
- Constraints:
  - Toolchain `g++` on this machine is GCC **16.1.1**, *unsuffixed*; the documented
    `g++-16` symlink is absent. The skeleton's toolchain definition must detect
    either name.
  - Project standard is now **C++26**. xmake's `set_languages("c++26")` is supported
    by xmake ≥ 2.9. GCC 16.1 has `-std=c++26` stable.
  - Slice 0 does not pull `fmt`, `nlohmann_json`, `asio`, etc. — they land with the
    libraries that need them. The PCH for slice 0 reflects that.
- Compile-budget impact (if any):
  - `oran-core` category target: median 0.8 s / p95 1.5 s / hard cap 2.0 s. Slice 0
    should land well under: the only TU is `src/oran-core/error.cpp` + the PCH.
  - Clean-build target: ≤ 30 s; expected actual for slice 0 is ≪ 30 s because nothing
    else compiles.

## Risks

- Risk: **GCC 16.1 named `g++`, not `g++-16`.** Mitigation: toolchain.lua probes both
  names with `find_program` and binds to whichever exists.
- Risk: **xmake's `c++26` flag has unexpected GCC fallout.** Mitigation: also try
  `cxx26`; fall back to passing `-std=c++26` directly in the toolchain's `cxxflags`
  if `set_languages` doesn't emit it.
- Risk: **`std::print` link order on libstdc++.** Mitigation: `<print>` is fully
  inline in GCC 16.1; no extra link flag needed. Validated by `xmake run orangutan`.
- Risk: **Catch2/nanobench not present in xmake-repo at pinned versions.** Mitigation:
  if `xmake require` for the pinned version fails, use the latest version available
  and update `docs/rules/libraries.md` in this slice.
- Risk: **`-fanalyzer` adds too much noise or slow compile.** Mitigation: enable only
  on `xmake build --analyze` flag (off by default); documented in the new
  `docs/rules/static-analysis.md`.

## Milestones

1. Rule sync: critical-rules / code-style / core-beliefs / BUILD_SYSTEM /
   module-and-pch / AGENTS conventions all carry C++26 + `std::print` + the new
   static-analysis rule.
2. Skeleton wiring: `xmake.lua`, `xmake/*.lua`, PCH, `include/oran/core/*`,
   `src/oran-core/*`, `src/main.cpp`.
3. Verification: tests/core and bench/core green; `xmake run orangutan` greets.
4. Rollout: QUALITY_SCORE row bump, release note, history entry, exec plan moved
   to `docs/exec-plans/completed/`.

## Validation

- Commands:
  - `xmake f -m release -y`
  - `xmake -j$(nproc)`
  - `xmake run orangutan`
  - `xmake build test-core && xmake run test-core`
  - `xmake build bench-core && xmake run bench-core`
  - `make ci`
  - `scripts/check-lib-parity.sh`
  - `scripts/check-docs-sync.sh`
- Manual checks:
  - `g++ --version` confirms GCC 16.1.1.
  - Confirm `compile_commands.json` is emitted at repo root for clangd.
  - Confirm `<print>` lands without explicit `-lstdc++exp` (slice 0 acceptance).
- Observability checks: none yet (no logging library).
- Bench comparison: `bench/core` A/B: `Error` built via builder + fluent `.with` vs.
  built via the move-only constructor with a pre-filled context vector. Record both
  median ns in the history entry.

## Progress Log

- [x] Confirm scope and constraints with the user (`/effort max`, "use C++26",
      "GCC 16.1", "std::print", "可以把一些规则写入规则文件").
- [x] Update `docs/rules/critical-rules.md` (new C17 + C18 rows).
- [x] Update `docs/rules/code-style.md` ("Console And Formatted Output" section).
- [x] Add `docs/rules/static-analysis.md`; reference it from `docs/rules/README.md`
      and `BUILD_SYSTEM.md`.
- [x] Update `docs/design-docs/core-beliefs.md` ("C++26 Engineering Principles").
- [x] Update `docs/BUILD_SYSTEM.md` (toolchain auto-detect, `c++26`, `--analyze=y`).
- [x] Update `docs/rules/module-and-pch.md` (PCH set + per-slice staging note).
- [x] Update `AGENTS.md` conventions row.
- [x] Implement `xmake.lua` + `xmake/*.lua`.
- [x] Implement `include/oran/_pch.hpp`, `include/oran/core/error.hpp`,
      `include/oran/core/result.hpp`, `src/oran-core/error.cpp`.
- [x] Implement `src/main.cpp`.
- [x] Implement `tests/core/` and wire `xmake/tests.lua`.
- [x] Implement `bench/core/` and wire `xmake/bench.lua`.
- [x] Run `xmake f && xmake && xmake run orangutan && xmake run test-core &&
      xmake run bench-core`; numbers recorded in the history entry.
- [x] Run `make ci`; resolved drift (added `scripts/check-analyzer-coverage.sh`
      stub; renamed `Catch2` → `catch2` row in `libraries.md` so it matches
      `xmake/packages.lua` casing).
- [x] Updated `docs/QUALITY_SCORE.md` rows: Build C→B, Test D→C, Bench D→C,
      added Static-analysis C row.
- [x] Wrote history entry
      `docs/histories/2026-05/20260514-2214-mvp-build-skeleton-slice-0.md`.
- [x] Added release note: "Slice 0 — build skeleton, oran-core, greet binary".
- [x] Move this plan to `docs/exec-plans/completed/` (this commit).

## Decision Log

- 2026-05-14: **Standard = C++26.** User requested explicit ratchet from documented
  C++23. GCC 16.1 ships `-std=c++26` stable + `<print>` + `<expected>` + `<generator>`,
  so the project's "ratchet toward C++26 behind features that GCC 16.1 ships stable"
  rule from `core-beliefs.md` is satisfied repo-wide. Doc updates follow this decision.
- 2026-05-14: **Toolchain executable autoselect.** Toolchain probes `g++-16`
  first (the originally-documented name in `BUILD_SYSTEM.md`), then `g++`. On this
  machine `g++` resolves to GCC 16.1.1. Rationale: don't bake one-binary assumptions
  into the build.
- 2026-05-14: **PCH is stdlib-only in slice 0.** `<fmt/core.h>` and
  `<nlohmann/json_fwd.hpp>` from the documented PCH set land with the libraries
  that introduce those deps (e.g., `oran-log`, `oran-storage`). `module-and-pch.md`
  is updated to reflect the per-slice evolution.
- 2026-05-14: **`-fanalyzer` is opt-in via an xmake option.** Default-on would
  inflate clean-build wall-clock and clash with the compile-budget targets. Slice 0
  ships the wiring; CI adoption tracked separately when oran-async lands.

## Linked Artifacts

- Related design doc: [`../../ARCHITECTURE.md`](../../ARCHITECTURE.md),
  [`../../design-docs/core-beliefs.md`](../../design-docs/core-beliefs.md).
- Related product spec: none yet (slice 0 is infrastructure; the first product
  slice — MVP ReAct loop — is `product-specs/0001-core-react-loop.md`).
- PRs: (filled at PR open)
- History entry: `docs/histories/2026-05/<ts>-mvp-build-skeleton-slice-0.md`
  (created in milestone 4).
- Release note: `docs/releases/feature-release-notes.md`
  (entry added in milestone 4).
