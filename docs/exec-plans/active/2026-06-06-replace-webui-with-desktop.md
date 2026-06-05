# Replace the Web UI with a Slint Desktop App

> Status: active · Date: 2026-06-06 · Type: framework-docs refactor (no C++ build/logic
> change) · Owner: huxint

## Goal

Remove the browser **Web UI** surface from the Orangutan v2 prompt-framework
documentation and replace it with a native, in-process **desktop application** built on
**Slint** — modern, animated, low-memory, in the spirit of the Hermes / Codex / Claude
Code desktop apps. The Web UI was never implemented (no `src/oran-web`, no `web/`
frontend, `cpp-httplib` not wired into xmake), so this is a documentation + code-comment
refactor only; the `oran-desktop` library itself is built later in its own slice.

## Scope

- **In scope:**
  - Rewrite `product-specs/0007-web-ui.md` content as "0007 — Desktop App"
    (filename kept; see Decision Log).
  - Rename `docs/FRONTEND.md` → `docs/DESKTOP.md` (Slint dev/build/test guide) and
    update `scripts/check-docs.sh`'s required-file list to match.
  - Re-point every **interface** reference (`web UI` / `oran-web` / `--web` /
    `cpp-httplib` / web binary mode) to the desktop app across the framework docs.
  - Update **code comments** that mention the web UI.
  - Record Slint's library/compile-budget/supply-chain posture; remove cpp-httplib's.
- **Out of scope:**
  - Any C++ logic or build change. `oran-desktop` is a future implementation slice.
  - The functional **`web` config block** (`oran-config::WebConfig` + `parse_web` +
    `config.example.json` + `tests/config`): left intact and green; its migration to a
    `desktop` block is tracked as future work (see tech-debt-tracker).
  - Historical docs: `docs/histories/**`, `docs/releases/**`,
    `docs/exec-plans/completed/**` (incidental "web" mentions are provenance).
  - `oran-http` (libcurl client) and `oran-channel-webhook` — not the Web UI.
  - UI layout / pixel mockups — a later frontend concern.

## Context

- **Decisions (locked with the user):**
  - Framework: **Slint** (C++-native, declarative `.slint` → generated C++,
    GPU-rendered, first-class animations, low memory; fits the single-binary C++26
    architecture and the compile-budget discipline better than Qt).
  - Connection model: **in-process, local-only.** `oran-desktop` links into the
    `orangutan` binary and drives `agent::Loop` on the shared asio executor, reusing
    the CLI streaming `EventSink`. No HTTP server; `cpp-httplib` removed from the design.
    Remote browser access is intentionally dropped (channels remain the remote reach).
  - Naming: `oran-web` → `oran-desktop`; spec slot `0007` repurposed; `--web` →
    `--desktop`; `FRONTEND.md` → `DESKTOP.md`.
  - Daemon: headless `orangutan-server` keeps **channels + automation only**.
  - Health/metrics: **in-app panel + structured logs; no HTTP** (HTTP a future
    daemon-only option).
  - Legacy audit: update only the forward-looking "v2" notes; keep the historical
    description of the old codebase's web server.
- **Why Slint over alternatives:** Qt (heavy, slow compiles, licensing friction);
  Dear ImGui (debug-tool aesthetic); Tauri/Electron/webview (re-introduce web tech;
  Electron memory-heavy); Flutter/egui/iced (Dart/Rust — fracture the C++ single
  binary).
- **Slint caveats to record:** triple-licensed (GPLv3 / royalty-free / commercial —
  fine here, note it); the `.slint` compiler generates C++, so `oran-desktop` needs its
  own compile-budget row and the generated code stays confined to that library.
- **CI gates that constrain this change** (`make ci` → `scripts/ci.sh`):
  - `check-docs.sh` hardcodes required files incl. `docs/FRONTEND.md` and
    `docs/product-specs/0007-web-ui.md` → rename FRONTEND in that list; keep the 0007
    filename.
  - `check-docs-sync.sh` Check 3 greps `product-specs/index.md` for each spec's
    filename stem → keep the `0007-web-ui` link path in index.md (retitle only).
  - `check-status-fresh.sh` passes without a new history (STATUS already points at the
    newest history file).
  - `check-docs-sync.sh` Checks 6/7 are satisfied: `xmake/packages.lua` has no
    httplib/slint, `xmake/targets.lua` defines no `web` target.

## Risks

- Risk: docs claim "no web" while `oran-config` still parses a `web` block.
  Mitigation: scope the rename to the **interface**; keep config-field doc enumerations
  accurate (they still say `web`); track the config-block migration explicitly.
- Risk: renaming `FRONTEND.md` breaks `check-docs.sh`. Mitigation: update the
  required-file list in the same change; run `make ci`.
- Risk: a missed dangling `oran-web` / `--web` / "web UI" reference outside historical
  dirs. Mitigation: a final `rg` sweep excluding `histories/`, `releases/`,
  `exec-plans/completed/` and the deliberately-kept config block.
- Risk: lost remote browser access (persona 3). Mitigation: deliberate trade for
  low-memory/local-first; `PRODUCT_SENSE.md` updated; channels remain the remote story.

## Milestones

1. Rewrite `0007` + rename `FRONTEND.md`→`DESKTOP.md` + update `check-docs.sh`.
2. Sync interface references across architecture / rules / build / specs / design-docs /
   references / generated / tech-debt docs.
3. Sync code comments (`oran-tool`, `tests`, `tests/README.md`).
4. `make ci` green + dangling-reference sweep + commit on a `docs/…` branch.

## Validation

- Commands: `make ci` (docs, hygiene, docs-sync, status-fresh, deps, preamble,
  shell-parse) must pass.
- Manual checks:
  - `rg -n 'oran-web|cpp-httplib|--web\b|web UI|web-ui|orangutan-server.*web' docs/
    --glob '!docs/histories/**' --glob '!docs/releases/**'
    --glob '!docs/exec-plans/completed/**'` returns only the deliberately-kept
    config-block mentions and the historical legacy-audit description.
  - `docs/DESKTOP.md` exists; `docs/FRONTEND.md` gone; `scripts/check-docs.sh` lists
    `DESKTOP.md`.
  - `docs/product-specs/index.md` still links `0007-web-ui.md` (title "Desktop App").
- No C++ build/test needed (no logic change). Code-comment edits cannot affect
  compilation.

## Progress Log

- [x] Confirm scope, decisions, and CI constraints.
- [ ] Milestone 1: 0007 rewrite + DESKTOP.md rename + check-docs.sh.
- [ ] Milestone 2: framework-doc interface sync.
- [ ] Milestone 3: code-comment sync.
- [ ] Milestone 4: `make ci` green + sweep + commit.

## Decision Log

- 2026-06-06: **Keep the `0007-web-ui.md` filename**, rewrite only its title/body.
  Renaming would break the stale inbound links in the untouched historical dirs and the
  spec-index grep; the title becomes "0007 — Desktop App".
- 2026-06-06: **Do not refactor the `web` config code.** `WebConfig`/`parse_web`/
  `config.example.json`/`tests/config` are implemented and green and were out of the
  user's "docs + comments" scope; touching `config.example.json` without the parser
  would break `test-config`. The `web`→`desktop` config-block migration lands with the
  `oran-desktop` implementation slice and is tracked in `tech-debt-tracker.md`.
- 2026-06-06: **No new history entry / STATUS bump.** Per the user's minimal-scope
  instruction; `check-status-fresh` stays green because STATUS already points at the
  newest history. (Can be added on request.)

## Linked Artifacts

- Product spec: `docs/product-specs/0007-web-ui.md` (rewritten as Desktop App).
- Guide: `docs/DESKTOP.md`.
- Design doc: `docs/design-docs/agent-platform.md` (interface table + roadmap).
- History entry: intentionally omitted (see Decision Log).
