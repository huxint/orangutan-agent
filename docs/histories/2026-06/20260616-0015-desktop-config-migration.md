## [2026-06-16 00:15] | Task: oran-desktop Slice B — `web` → `desktop` config migration

### Execution Context

- Agent: `claude-code`
- Base model: `claude-opus-4-8[1m]`
- Runtime: Claude Code CLI, GCC 16.1 / xmake, WSL2 (Arch). Test-driven.
- Linked plan: `docs/exec-plans/active/2026-06-14-oran-desktop-chat-tracer.md` (Slice B)

### User Query

> Continue the desktop track in order. Slice B: migrate the legacy networked
> `web` config block to a `desktop` UI-preferences block.

### Changes Overview

- Areas: `oran-config` (typed config + parser), `config.example.json`,
  `tests/config`, `oran-bootstrap` (startup summary line).
- Key actions:
  - Replaced `config::WebConfig { enabled, bind, port }` with
    `config::DesktopConfig { enabled, theme, reduce_motion }` and the
    `Config::web()` accessor with `Config::desktop()`.
  - Replaced `parse_web` with `parse_desktop`, mirroring the modern section
    pattern: type-checked fields, `theme` validated against
    {`system`, `light`, `dark`}, and `collect_unknown_object_fields` warnings
    (`"unknown desktop field"`) — `web` is no longer a recognized root field, so a
    leftover `web` block now warns (loose) / errors (strict).
  - `config.example.json`: `web` block → `desktop` block.
  - `oran-bootstrap` startup summary: `web=<enabled|disabled>` →
    `desktop=<enabled|disabled>`. Binary version bumped to `2.0.0-slice249`.

### Design Intent

The `web` block was a vestige of the removed `cpp-httplib` browser Web UI
(`enabled`/`bind`/`port` for a network listener). The product is now an
in-process, local-only Slint desktop app with no HTTP server, so a networked
config block is meaningless. `DesktopConfig` models the actual surface
(spec 0007 / `DESKTOP.md`): on/off, system-following theme, and a reduce-motion
accessibility toggle. `theme` is validated and unknown fields warn, matching the
config strictness sweep (slice 151) rather than the older permissive `parse_web`.
The bridge/view-model that consumes these preferences lands in Slice C.

### Files Modified

- `include/oran/config/config.hpp` — `DesktopConfig`, `desktop()`, `desktop_`.
- `src/oran-config/config.cpp` — recognized root field `web`→`desktop`,
  `kRecognizedDesktopFields` / `kRecognizedDesktopThemes`, `parse_desktop`,
  `parse()` integration.
- `config.example.json` — `web` block → `desktop` block.
- `src/oran-bootstrap/bootstrap.cpp` — summary line; version 249.
- `tests/config/test_config.cpp` — desktop fixture + assertions; new
  "handles the desktop block" case (defaults, values, theme/reduce_motion
  validation, unknown-field warning).

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/ARCHITECTURE.md` — `oran-config` typed-field lists `web`→`desktop` (+ the
  slice-status blockquote and the spec-0007 status note); new `desktop` fields.
- `docs/product-specs/0007-web-ui.md` — Config section: the `desktop` block is
  now shipped (`DesktopConfig`), the legacy `web` block is gone.
- `docs/design-docs/secrets-and-state.md` — config skeleton + typed-field list
  `web`→`desktop`.
- `docs/exec-plans/tech-debt-tracker.md` — 2026-06-06 desktop row removed (fully
  resolved: library shipped in Slice A, config migration in Slice B).
- `docs/STATUS.md`, `docs/ROADMAP.md` — slice 249, frontier → Slice B shipped /
  next = Slice C, library surfaces, tech-debt list.

### Validation

- TDD: wrote the desktop config tests first and watched `test-config` fail to
  compile (`Config has no member 'desktop'`), then implemented to green.
- `xmake run test-config` → **56 cases / 537 assertions** pass (was 55 / 519).
- `xmake run test-bootstrap` → 156 cases / 1573 assertions pass.
- `xmake build orangutan` (gated `--desktop=y`) links; `--config config.example.json`
  prints `config summary: … desktop=disabled`.
- `make ci` green.

### Follow-ups

- Next: Slice C — always-built bridge + `ChatViewModel` + `DesktopEventSink`,
  injecting `provider::EventSink*` into `AgentPromptRunner`; `tests/desktop`
  ≥60% coverage with a fake provider.
- Tech-debt: 2026-06-06 desktop row closed.
- Linked release note: deferred to Slice D (user-visible chat).
