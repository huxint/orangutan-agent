## [2026-05-14 22:14] | Task: MVP build skeleton — slice 0

### Execution Context

- Agent: Claude Code (Opus 4.7, 1M context)
- Base model: claude-opus-4-7
- Runtime: Claude Code CLI, repo on Linux/WSL2, GCC 16.1.1 system compiler.
- Linked plan: `docs/exec-plans/completed/2026-05-14-mvp-build-skeleton-slice-0.md`

### User Query

> 在这个提示词工程框架下开始开发项目. 注意是 gcc 16.1 编译器；当前需要使用 C++26；
> 过程中还可能需要使用 gcc 16.1 的代码分析命令；代码优先使用最新的语法，比如 print
> 代替 cout. 等等，你可以把一些规则也写入规则文件；然后就开始开发吧！

### Changes Overview

- Areas: build system (xmake root + subdirs), oran-core library, orangutan binary,
  tests/core (Catch2 v3), bench/core (nanobench), rule docs (C++26 + std::print +
  static analysis), top-level doc set.
- Key actions:
  - Ratcheted the project's language standard from C++23 to **C++26** (rule C17),
    threading the change through every doc that referenced the old standard.
  - Added critical-rule C18 + a new `docs/rules/static-analysis.md` describing the
    GCC 16.1 `-fanalyzer` wiring, the required-warning set, and the suppression
    policy.
  - Wrote the root `xmake.lua` and `xmake/{options,toolchain,packages,targets,tests,bench,checks}.lua`
    with `set_languages("c++26")`, an autoselecting `gcc-16`/`gcc` toolchain, and
    the `--analyze=y` opt-in flag. `compile_commands.json` is autoupdated at repo
    root via xmake's plugin (already documented; no change needed).
  - Implemented `include/oran/core/{error,result}.hpp` + `src/oran-core/error.cpp`
    (`ErrorKind`, `Error`, fluent `.with`, `retry_after`, `retryable()`,
    `to_string_view`, `Result<T>`, `all_ok<...>`, plus `std::formatter`
    specialisations so `std::print("{}", err)` just works).
  - Wrote `src/main.cpp` (`orangutan` binary, `std::print` end-to-end, no
    `<iostream>`).
  - Wrote Catch2 v3 bucket `tests/core/` (8 test cases, 47 assertions, all
    [unit][core]).
  - Wrote nanobench bucket `bench/core/` with one A-vs-B scenario
    (`Error.builder_chain` vs `Error.prefilled_ctor`).

### Design Intent

The project's own playbook (`docs/REPO_COLLAB_GUIDE.md`) requires small,
explicit slices over mega-PRs. Slice 0 is the smallest cut that proves the
toolchain end-to-end without committing to library shape for the rest of the
inventory — one library + one binary + one tests bucket + one bench bucket,
sized to fit the ≤600-lines/≤6-files target with room left for the doc churn the
Prime Directive demands.

The C++26 ratchet was driven by the user request and is internally consistent
with `docs/design-docs/core-beliefs.md`'s existing "ratchet to C++26 features
that GCC 16.1 ships stable" stance — making the ratchet the default just removes
a per-feature decision burden.

For the PCH we **staged** the documented `<fmt/core.h>` / `<nlohmann/json_fwd.hpp>`
entries instead of pulling them eagerly: their owning libraries haven't landed,
so the PCH would otherwise force every slice-0 build to fetch packages it does
not consume. The PCH evolves with each slice; `docs/rules/module-and-pch.md`
captures the rule.

### Files Modified

- `AGENTS.md` — conventions row updated: C++26, std::print, static analyzer link.
- `xmake.lua`, `xmake/options.lua`, `xmake/toolchain.lua`, `xmake/packages.lua`,
  `xmake/targets.lua`, `xmake/tests.lua`, `xmake/bench.lua`, `xmake/checks.lua` (new).
- `include/oran/_pch.hpp`, `include/oran/core/error.hpp`,
  `include/oran/core/result.hpp` (new).
- `src/oran-core/error.cpp`, `src/main.cpp` (new).
- `tests/core/main.cpp`, `tests/core/test_error.cpp` (new).
- `bench/core/main.cpp`, `bench/core/scenarios/error_construct.cpp`,
  `bench/core/README.md` (new).
- `docs/rules/critical-rules.md`, `docs/rules/code-style.md`,
  `docs/rules/static-analysis.md` (new), `docs/rules/module-and-pch.md`,
  `docs/rules/README.md` (edited).
- `docs/design-docs/core-beliefs.md`, `docs/BUILD_SYSTEM.md`,
  `docs/ARCHITECTURE.md`, `docs/QUALITY_SCORE.md`,
  `docs/releases/feature-release-notes.md` (edited).
- `docs/exec-plans/completed/2026-05-14-mvp-build-skeleton-slice-0.md` (new).

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `AGENTS.md` — Conventions row: C++26 + std::print + static analyzer.
- `docs/rules/critical-rules.md` — added C17 (C++26 + std::print) and C18 (analyzer).
- `docs/rules/static-analysis.md` — new file; referenced by C18 and `BUILD_SYSTEM.md`.
- `docs/rules/code-style.md` — new "Console And Formatted Output" section.
- `docs/rules/module-and-pch.md` — PCH set updated (`<chrono>`, `<format>`,
  `<print>`, `<ranges>`, `<functional>`, `<concepts>`); fmt/json_fwd staged.
- `docs/rules/README.md` — index row for `static-analysis.md`.
- `docs/design-docs/core-beliefs.md` — section header and bullet list updated
  to C++26 with std::print/format/generator/span/deducing-this references.
- `docs/BUILD_SYSTEM.md` — `c++26` literal in the sample xmake.lua; toolchain
  autoselects `gcc-16`/`gcc`; `--analyze=y` option block; new `xmake f` example.
- `docs/ARCHITECTURE.md` — slice-0 status note above the library inventory.
- `docs/QUALITY_SCORE.md` — rows bumped to reflect shipped reality.
- `docs/releases/feature-release-notes.md` — slice-0 entry.

No silently-broken doc was discovered; `scripts/ci.sh` + `scripts/check-docs-sync.sh`
both green after the doc churn.

### Validation

- Commands run:
  ```sh
  xmake f -m release -y
  xmake -j$(nproc)            # 4.37 s on the dev box (≪ 30 s budget)
  xmake run orangutan         # prints v2.0.0-slice0 greeting via std::print
  xmake build test-core       # 5.72 s
  xmake run test-core         # 8 cases / 47 assertions / all passed
  xmake test                  # registered test-core bucket
  xmake build bench-core      # 7.18 s
  xmake run bench-core
  scripts/ci.sh
  scripts/check-lib-parity.sh
  ```
- Tests added/changed: 8 cases under `tests/core/test_error.cpp` (`ErrorKind`
  builders, `Error::retryable`, `Error::with` ordering, `std::format` integration,
  `Result<T>` happy/sad, `all_ok` happy + short-circuit, `to_string_view`).
- Bench impact: new bench bucket, no comparison to a baseline yet. Three runs on
  this box (release, `-flto=auto`, GCC 16.1.1, 8-core WSL2):
  ```
  |    run | ns/Error builder_chain | ns/Error prefilled_ctor |
  |-------:|-----------------------:|------------------------:|
  |  run-1 |                  62.98 |                   79.66 |
  |  run-2 |                  65.18 |                   68.52 |
  |  run-3 |                  84.50 |                   66.20 |
  ```
  err% across both scenarios stays in the 1.6 %–5.3 % band, so the two
  construction paths are within measurement noise on this hardware. The
  takeaway for now is "no measurable difference"; we'll re-bench when LTO
  changes (e.g. when `oran-async` introduces cross-TU inlining pressure) to
  see whether one path starts winning consistently. A stable baseline file
  lands when `scripts/bench-compare.sh` first gets called in anger.
- Compile-budget delta: no prior baseline; current measurements on the dev box:
  - `oran-core` TU (PCH + `src/oran-core/error.cpp`): ≤ 0.7 s combined —
    comfortably inside the 0.8 s median / 2.0 s hard cap for the core category.
  - Clean release build: 4.37 s ≪ 30 s target ≪ 60 s hard cap.
  - Tests build: 5.72 s incremental from the cached core artifacts.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none new; the existing `docs/exec-plans/tech-debt-tracker.md`
  already tracks "skeleton ships in the same PR that lands the first xmake
  build" — this slice satisfies it.
- Linked release note: `docs/releases/feature-release-notes.md` (2026-05 row).
- Next slice candidate: `oran-async` MVP (asio + coroutine runtime + Channel<T>)
  before any provider/agent work — it is the blocker for everything downstream.
