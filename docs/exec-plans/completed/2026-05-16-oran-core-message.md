# `oran-core` — Conversation Types Slice (`Role`, `Content`, `Message`, `StopReason`)

## Goal

Land the second `oran-core` data-type slice: the conversation-layer
primitives every higher layer (`storage`, `memory`, `provider`, `agent`,
`channel`, `cli`) is currently blocked on. The slice ships `core::Role`,
`core::Content` (a `std::variant` of `TextContent`, `ThinkingContent`,
`ToolUseContent`, `ToolResultContent`), `core::Message`, and
`core::StopReason`, with stable `to_string_view` mappings and a few
convenience helpers. No JSON serializer is included — that responsibility
belongs to the protocol adapters.

## Scope

- In scope:
  - `include/oran/core/role.hpp` and `src/oran-core/role.cpp`:
    `Role { user, assistant, system, tool }` plus
    `to_string_view(Role) noexcept` and `std::formatter<Role>`.
  - `include/oran/core/stop_reason.hpp` and `src/oran-core/stop_reason.cpp`:
    `StopReason { end_turn, max_tokens, tool_use, stop_sequence, cancelled,
    error }` plus `to_string_view(StopReason) noexcept` and
    `std::formatter<StopReason>`.
  - `include/oran/core/content.hpp` and `src/oran-core/content.cpp`:
    `TextContent`, `ThinkingContent`, `ToolUseContent`, `ToolResultContent`
    structs and the `Content` variant alias. JSON payloads stay as opaque
    `std::string` (no nlohmann include in this header). Equality is
    member-wise (`= default`). Visit helpers:
    `holds_text`, `holds_thinking`, `holds_tool_use`, `holds_tool_result`,
    and `text_view(const Content&) -> std::optional<std::string_view>`
    that returns the inner text for a `TextContent` and `std::nullopt`
    otherwise.
  - `include/oran/core/message.hpp` and `src/oran-core/message.cpp`:
    `Message` struct (`role`, `blocks`, optional `created_at`) with
    member-wise equality, plus two convenience builders
    `Message::user_text(std::string)` and
    `Message::assistant_text(std::string)`.
  - Tests:
    - `tests/core/test_role.cpp`
    - `tests/core/test_stop_reason.cpp`
    - `tests/core/test_content.cpp`
    - `tests/core/test_message.cpp`
  - Bench: `bench/core/scenarios/message.cpp` with an A/B between
    `std::visit(Overloaded{...}, content)` and
    `std::get_if<TextContent>(&content)` for the common text-only path,
    plus a baseline that walks a 32-block conversation.
  - Docs: `docs/ARCHITECTURE.md` inventory + slice-status,
    `docs/QUALITY_SCORE.md` test/bench rows, `bench/core/README.md`,
    `docs/releases/feature-release-notes.md`, history entry.
- Out of scope:
  - `core::ToolDef` (lives in a later slice with the tool registry).
  - `core::str` UTF-8 helpers (separate future slice).
  - JSON encoders/decoders for `Content` (will land with the first
    `oran-provider` adapter).
  - Rewiring `SessionRepository` to take `Message` directly — kept as a
    follow-up to keep this slice within the C14 size guideline.

## Context

- Relevant docs:
  - `docs/ARCHITECTURE.md` (core inventory lists these types).
  - `docs/design-docs/module-boundaries.md` (explicitly puts `Message`,
    `Content`, `Role`, `StopReason` in `oran-core`).
  - `docs/design-docs/api-portability.md` (`provider::Request` /
    `Response` consume these types).
  - `docs/design-docs/memory-system.md` (`memory::session::Store` accepts
    `core::Message`).
  - `docs/rules/code-style.md` ("Variants Over Inheritance" — `Content`
    must be a `std::variant`, visited with an `Overloaded` set).
  - `docs/rules/critical-rules.md` (C7 — `explicit` constructors; C17 —
    prefer modern stdlib facilities).
- Relevant code paths:
  - `include/oran/core/`
  - `src/oran-core/`
  - `tests/core/`
  - `bench/core/`
- Constraints:
  - Pure stdlib; no new third-party packages.
  - Public headers stay light: `<string>`, `<string_view>`, `<vector>`,
    `<variant>`, `<optional>`, `<cstdint>`, and `<oran/core/time.hpp>` /
    `<oran/core/error.hpp>` where actually needed. No JSON include.
  - `Content` JSON payloads stay opaque strings. The variant alternatives
    are pure values — no `unique_ptr`, no PIMPL.
- Compile-budget impact (if any):
  - Four small public headers and four implementation TUs. The largest new
    public include is `<variant>` (which the project already needs).
  - One bench scenario TU and four test TUs added.

## Risks

- Risk: ripple effect on TUs that already include `<oran/core/error.hpp>`
  through the umbrella `<oran/core.hpp>` if it grows to re-export these.
  Mitigation: do **not** add an umbrella re-exports header in this slice.
  Each new file is opt-in; the current scaffolds stay unaffected.
- Risk: `Content` equality is naive `= default`, which makes it depend on
  string equality across very large JSON payloads. Mitigation: members are
  `std::string`, so default equality is fine; document that equality is
  the cheap path callers expect.
- Risk: bench A/B between `std::visit` and `std::get_if` becomes a "modern
  C++ is slower" footgun cited out of context. Mitigation: include the
  walk-all-block baseline so the result describes the shape of the
  difference (single-alternative shortcut vs. branching over the full
  variant) rather than reading as "don't use visit".
- Risk: introducing `Message::user_text` / `Message::assistant_text`
  invites callers to bypass the `blocks` vector. Mitigation: explicitly
  document the helpers as test/fixture sugar; production code constructs
  `Message{ role, blocks }` directly.

## Milestones

1. Add active plan, settle API, get test scaffolding green.
2. Implement enums (`Role`, `StopReason`) with stable `to_string_view`.
3. Implement `Content` variant + helpers; cover with tests.
4. Implement `Message` + helpers; cover with tests.
5. Add the bench scenario and register it in `bench/core/main.cpp`.
6. Update docs (architecture, quality score, bench README, release notes,
   history).
7. Run validation and move plan to `completed/`.

## Validation

- Commands:
  - `xmake build test-core && xmake run test-core`
  - `xmake build bench-core && xmake run bench-core`
  - `xmake test`
  - `xmake build orangutan`
  - `git diff --check`
  - `make ci`
- Manual checks:
  - Confirm public headers add only the stdlib + `oran/core/*` includes
    listed above.
  - Confirm `Message`/`Content` equality follows member-wise semantics.
  - Confirm there is no JSON dependency anywhere in `include/oran/core/`.
- Observability checks:
  - None (pure value types).
- Bench comparison (if perf-relevant):
  - `core.content_visit_overloaded` vs. `core.content_get_if_text`:
    measures the cost of the project-preferred `std::visit` style against
    a single-alternative fast path. The result will be reported as a
    ratio, not a pass/fail.
  - `core.message_walk_blocks`: walks a 32-block conversation; records
    the cost of the common "render this turn's content to a string" path.

## Progress Log

- [x] Confirm scope and constraints.
- [x] Implement `Role`, `StopReason`, `Content`, `Message`.
- [x] Add core tests.
- [x] Add core bench scenario and registration.
- [x] Update docs that this slice invalidates in the same PR.
- [x] Run validation and record results.
- [x] Write history entry.
- [x] Add release note.
- [x] Move plan to `docs/exec-plans/completed/` before finish.

## Decision Log

- 2026-05-16: keep `Content` JSON payloads as opaque `std::string` rather
  than `nlohmann::json`. Rationale: the public header in `oran-core` must
  stay nlohmann-free (rule C6); protocol adapters in `oran-provider` are
  the right place to parse vendor JSON into `Content` blocks.
- 2026-05-16: include `created_at` (`std::optional<Time>`) on `Message`.
  Rationale: the just-landed `core::Time` makes this cheap; `storage` and
  `memory` already need to plumb a timestamp per message and the typed
  field keeps the wire-shape conversation in one place.
- 2026-05-16: ship `Message::user_text` / `Message::assistant_text`.
  Rationale: every test and most bootstrap uses build a single-text-block
  message; the helpers prevent five-line ceremony from leaking into every
  call site without hiding the underlying vector.

## Linked Artifacts

- Related design doc: `docs/design-docs/api-portability.md`,
  `docs/design-docs/memory-system.md`, `docs/design-docs/module-boundaries.md`.
- Related product spec: `docs/product-specs/0001-core-react-loop.md`,
  `docs/product-specs/0005-memory-system.md`.
- PRs: local change.
- History entry: `docs/histories/2026-05/20260516-1605-oran-core-message.md`
- Release note: `docs/releases/feature-release-notes.md`
