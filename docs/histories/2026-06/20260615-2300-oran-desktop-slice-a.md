## [2026-06-15 23:00] | Task: oran-desktop Slice A — Slint packaging + gated skeleton window

### Execution Context

- Agent: `claude-code`
- Base model: `claude-opus-4-8[1m]`
- Runtime: Claude Code CLI, GCC 16.1 / xmake, WSL2 (Arch). Slice-A code was
  authored in session `6cab25d3` (2026-06-14); this entry closes the slice —
  re-verifying both build paths, syncing docs (Prime Directive), and committing.
- Linked plan: `docs/exec-plans/active/2026-06-14-oran-desktop-chat-tracer.md`

### User Query

> Start building the desktop version of this project. (Continued across sessions;
> this slice closes out Slice A: sync docs + history, verify, and commit.)

### Changes Overview

- Areas: new `oran-desktop` interface-layer library; xmake build wiring (options,
  packages, targets, tests, bench); `oran-bootstrap` `--desktop` flag; docs.
- Key actions:
  - New library `oran-desktop` (always built): `desktop::gui_compiled()`
    build-config accessor plus the unconditionally-declared, gated-defined
    `desktop::shell::run()` window entry. No Slint type leaks through public
    headers (C6).
  - Gated Slint UI shell under `src/oran-desktop/shell/` + `ui/app_window.slint`,
    compiled only with `xmake f --desktop=y`.
  - Custom `package("slint")` consuming the official **prebuilt C++ binary**
    release v1.16.1 (sha256-pinned) — headers + `libslint_cpp.so` +
    `slint-compiler`; no Rust toolchain in any build path.
  - `--desktop` build option (default off) and a `.slint`→C++ codegen
    `before_build` rule on the `oran-desktop` target (incremental via
    `depend.on_changed`), output confined to the target's autogen dir.
  - `orangutan --desktop` runtime flag in `oran-bootstrap`: launches the window in
    gated builds, errors with "rebuild with `--desktop=y`" otherwise. Binary
    version bumped to `2.0.0-slice248`.
  - `tests/desktop` + `bench/desktop` buckets (C12 parity).

### Design Intent

Two-target-style split inside one xmake target: the bridge/view-model surface is
pure C++ and **always built + tested**, while the heavy Slint toolkit and the
generated code compile **only** under `--desktop=y`. This keeps the GUI toolkit's
compile/link cost off the default build (rule L4 / compile budget — the previous
project failed here) while keeping desktop logic in default CI. Slint is consumed
as prebuilt binaries (not a CMake/Rust source build) so the only desktop compile
tax is the generated code. Slice A deliberately front-loads the packaging +
codegen + link integration unknown before any streaming/bridge work depends on it;
the runtime bridge, view-model, and `provider::EventSink` marshalling arrive in
Slices C/D. Decisions are recorded in the exec-plan's Decision Log.

### Files Modified

- `include/oran/desktop/desktop.hpp`, `include/oran/desktop/shell.hpp` (new)
- `src/oran-desktop/desktop.cpp`, `src/oran-desktop/shell/shell.cpp`,
  `src/oran-desktop/ui/app_window.slint` (new)
- `tests/desktop/test_desktop.cpp`, `tests/desktop/main.cpp` (new)
- `bench/desktop/main.cpp`, `bench/desktop/README.md` (new)
- `src/oran-bootstrap/bootstrap.cpp` (`--desktop` flag, usage line, version 248)
- `xmake/options.lua`, `xmake/packages.lua`, `xmake/targets.lua`,
  `xmake/tests.lua`, `xmake/bench.lua`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/rules/libraries.md` — moved `slint` to the Optional (Feature-Gated) table,
  pinned at `1.16.1` (`--desktop=y`), noted prebuilt-binary / GPL-3.0-or-later.
- `docs/SUPPLY_CHAIN_SECURITY.md` — `slint` as a gated optional native dep +
  prebuilt-binary provenance (canonical upstream URL, sha256 pin, no Rust).
- `docs/BUILD_SYSTEM.md` — `desktop` option in "Shipped so far"; `oran-desktop`
  target shape (always-built half + gated shell + codegen rule).
- `docs/DESKTOP.md` — status → in-progress (Slice A); `--desktop=y` configure step
  in Local Dev; current-vs-target dependency note.
- `docs/ARCHITECTURE.md` — `oran-desktop` library-inventory row (Slice-A surface;
  deps `currently oran-core; planned oran-agent, oran-orchestration`).
- `docs/ROADMAP.md` — Desktop row frontier → Slice A shipped, next = Slice B;
  Dependency Frontier #6 note.
- `docs/exec-plans/tech-debt-tracker.md` — 2026-06-06 desktop row narrowed
  (library + version-pin closed; `web`→`desktop` config migration still open).
- `docs/STATUS.md` — slice 248, last history, active exec-plans, library surfaces,
  tech-debt sync.

### Validation

- Commands run (this slice):
  - `xmake f -m release --desktop=n` → built `oran-desktop`, `test-desktop`,
    `orangutan`; `xmake run test-desktop` (passes, `gui_compiled()==false`);
    `orangutan --desktop` exits 1 with the rebuild error; `--help` lists
    `--desktop`.
  - `xmake f -m release --desktop=y` → custom `slint` package installs;
    `slint-compiler` generates `app_window.h`; `oran-desktop` shell compiles;
    `orangutan` links `libslint_cpp.so`; `xmake run test-desktop` passes
    (`gui_compiled()==true`).
  - `make ci` (docs/hygiene/docs-sync/status/deps/preamble).
- Tests added/changed: `test-desktop` — 1 case / 1 assertion (build-config accessor
  adapts via `ORAN_ENABLE_DESKTOP`).
- Bench impact: `bench-desktop` placeholder (no measurable bridge workload yet).
- Compile-budget delta: default build unchanged (Slint absent); the gated
  `oran-desktop` shell carries the only desktop compile tax (generated code).

### Follow-ups

- Issues opened: none.
- Tech-debt entries: 2026-06-06 desktop row — `web`→`desktop` config migration
  remains open (Slice B).
- Live window confirmed: `orangutan --desktop` initializes the Slint backend and
  runs the event loop on WSLg/Xwayland under both the default (femtovg/EGL,
  software-rendered) and `winit-software` renderers — no font/backend/xlib errors,
  only a non-fatal libEGL DRI3 software-render notice and an absent-D-Bus-portal
  color-scheme warning. (On-screen pixels are not visually inspectable from a
  headless agent; evidence is clean backend init + a sustained event loop.) The
  prior session's font / X11-client-lib / `linuxkms` blockers are all resolved.
- Linked release note: deferred to Slice D, when `orangutan --desktop` becomes a
  user-visible working chat.
