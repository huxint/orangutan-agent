# 0014 — Structured Tool Output (`ToolOutput` v2)

## User Problem

`tool::Output` used to be `{ std::string text; bool is_error; }`.
Slice 60 promotes it into a forward-compatible envelope in
[`include/oran/tool/output.hpp`](../../include/oran/tool/output.hpp);
the remaining problem is that most built-ins and every future provider
adapter still consume only the text fallback. Current built-ins still
flatten structured facts into prose:

- `file.read` returns the file body as text; range metadata, fingerprint,
  and truncation flag (spec 0011) have nowhere to live except a
  hand-rolled header line.
- `file.search` returns one `<path>:<line>: <preview>` line per match; the
  match count, byte budget consumed, and per-file fingerprints are
  invisible.
- The future `code.outline` / `code.symbols` (per
  [`../design-docs/tool-runtime.md`](../design-docs/tool-runtime.md)
  "Built-in Tool Categories" and the deep-review §Tool design judgment)
  needs to return a structured symbol tree — converting it to markdown in
  the handler then reparsing it in the agent is a downstream-of-the-LLM
  anti-pattern.
- `memory.recall` will need to distinguish *the records retrieved* from
  *the summary surfaced to the model*; today that is one text blob.

The cost is paid at four call sites:

1. **Provider adapters** re-parse strings to derive structure they need
   for vendor tool-result blocks.
2. **The web UI and channel adapters** lose the ability to render
   structured payloads (file diffs, symbol trees, match groups) without
   downstream parsers.
3. **Audit and hooks** see only the rendered text — bytes-touched,
   files-changed, cost-estimate cannot fan out cleanly.
4. **The cache layer** (spec 0011 line-offset index, spec 0012 rendered
   tool-block cache) has no stable identity to cache against because the
   rendered text is the only output.

This spec defines the v2 output contract. Every tool — current and
future — returns the same envelope; provider adapters, UI, audit, and
caches all consume one shape.

## Scope (v1)

The MVP delivers a forward-compatible envelope that today's text-only
handlers can adopt without churning every callsite at once.

> **Status (slice 60, 2026-05-24):** the base envelope now ships in
> `oran-tool`. `Output::text_only(...)` preserves the existing text path,
> `Output::error(...)` marks structured-capable errors, `ToolAfterPayload`
> copies `Output::usage` on successful dispatch, and `bench-tool` has
> `output.text_only` vs. `output.with_data_16kib` coverage. Provider
> adapter mapping, scheduler byte caps, audit usage fan-out, hook raw-data
> redaction, and built-in structured payload migrations remain downstream.

- **`tool::Output v2`**:
  ```cpp
  // include/oran/tool/output.hpp — PUBLIC
  struct ToolUsage {
    std::optional<std::uintmax_t>  bytes_read;
    std::optional<std::uintmax_t>  bytes_written;
    std::optional<std::uint32_t>   files_touched;
    std::optional<std::uint64_t>   match_count;
    std::optional<double>          cost_estimate;
    std::optional<std::chrono::nanoseconds> wall_time;
    bool                           truncated{false};
    bool                           data_dropped{false};
  };

  struct Attachment {
    std::string                    file_path;
    std::string                    mime_type;
    std::optional<std::uintmax_t>  byte_size;
    std::optional<std::string>     fingerprint;
  };

  struct Output {
    std::string                              text;          // required, model-facing summary
    std::optional<std::string>               data_json;     // serialized structured JSON bytes
    std::vector<Attachment>                  attachments;   // file / image / blob metadata
    ToolUsage                                usage;         // metrics; defaults to all-nullopt
    bool                                     is_error{false};
  };
  ```
  - `text` stays **required** so provider adapters always have a textual
    fallback. The conversion-from-v1 path is a `text`-only
    `Output::text_only(...)` construction with the other fields default.
  - `data_json` is serialized JSON, not a public `nlohmann::json` value.
    This keeps the public header third-party-free and preserves the
    compile budget per
    [`../rules/compile-budget.md`](../rules/compile-budget.md). Provider
    adapters own protocol-specific parsing / serialization later.
  - `Attachment` is a concrete metadata value for
    `{ file_path, mime_type, byte_size, fingerprint }`; tools keep the
    vector empty until a file / image / blob producer needs it.
  - `usage` defaults to all-nullopt; tools fill only the fields they
    measure. Audit fan-out and cost-aware scheduling (spec 0012) read
    from here.
- **Convenience helpers** for the common path:
  ```cpp
  Output Output::text_only(std::string text);
  Output Output::error(std::string message, std::optional<std::string> data_json = std::nullopt);
  ```
  Two helpers, not a fluent DSL. Tools that need structured output
  construct the value directly.
- **Provider adapter contract.** Each `protocol::Adapter` decides how to
  carry `Output::data_json` into its vendor format:
  - Anthropic Messages tool-result: `data_json` rides as the JSON `content`
    array when present; otherwise the adapter sends `text` as a single
    text block.
  - OpenAI Responses tool-result: `data_json` is serialised as JSON in the
    `output` field; otherwise the rendered `text`.
  - Gemini, custom-OpenAI-compatible: same pattern — JSON when
    `data_json.has_value()`, plain text otherwise.
  - Adapters never *invent* structure: the agent loop, not the adapter,
    decides what to send.
- **Audit fan-out.** `permission::AuditEvent` already carries a
  `context` JSON map; the dispatch pipeline records `usage` keys
  (`bytes_read`, `bytes_written`, `files_touched`, `match_count`,
  `cost_estimate`, `wall_time_ms`) when the tool returns them. The
  existing `input_hash` discipline is unchanged.
- **Hook fan-out.** `ToolAfterPayload` (already defined in slice 22 +
  slice 25) now has a `usage` field carrying the same metrics. The
  payload's `output_text` stays the truncated rendering used today;
  full structured `data_json` rides as a separate optional field only when
  the consuming sink declares the `kind::trusted_local` capability
  documented in the deep-review §Hook/audit redaction recommendation.
- **Byte caps**. Two caps, independent:
  - `runtime.tool_output.max_text_bytes` (default 256 KiB) — applies to
    `text`. Exceeding it truncates and sets `is_error=false` with a
    `truncated=true` flag captured in `usage`.
  - `runtime.tool_output.max_data_bytes` (default 1 MiB) — applies to
    serialised `data_json`. Exceeding it drops `data_json`, leaves `text` intact,
    and records `data_dropped=true` in `usage`.
  Caps fire in the scheduler (spec 0012), not in each handler.
- **Migration path.** Built-ins migrate one at a time:
  1. `file.read` (becomes v2's primary structured caller via spec 0011).
  2. `file.search` (gains structured `matches[]` and per-file usage).
  3. `directory.list` (gains structured `entries[]`).
  4. `file.write` / `file.edit` / `file.delete` (gain `usage.bytes_written`
     and `usage.files_touched`; `data_json` stays `nullopt` for v1).
  Until each migrates, its handler constructs `Output::text_only(...)`
  and behaves exactly as today.

## Scope (v1.1)

- **`Attachment` shape finalised** when the first tool produces one
  (likely a `file.read` binary fallback or a `code.outline` rendered
  graph). Until then `attachments` stays an empty vector — present in the
  envelope, absent in transport.
- **Cost estimation surface.** Tools that consult an external service
  (LSP, MCP) fill `usage.cost_estimate`; the scheduler aggregates it for
  cost-aware preemption (spec 0012 v2).
- **Structured error envelope.** `Output{ .is_error = true }` may carry
  `data_json = { "kind": "...", "context": { ... } }` so an agent can
  branch on error class without parsing prose. The kind set tracks
  `core::Error::Kind`.
- **`tool::parse_input<T>`** helper (tracked under the deep-review
  follow-up tech-debt row) lands alongside `tool::Output` v2 so handlers
  stop hand-rolling both input parsing *and* output construction.

## Scope (v2)

- Streaming tool output. Long-running tools (`shell.exec`, future
  `code.test`) emit chunks via a `tool::OutputSink` analogous to the
  provider streaming sink in
  [`../design-docs/api-portability.md`](../design-docs/api-portability.md).
  Closes when the agent loop's streaming path is mature enough that
  partial outputs don't break transcript ordering.
- Multi-language attachment rendering for the web UI (spec 0007).

## Out Of Scope

- A typed-variant alternative (`std::variant<TextOutput, FileReadOutput,
  SearchOutput, ...>`). Considered; rejected because every new tool
  would force a recompile of every consumer, breaking the compile
  budget. The serialized `data_json` channel is the escape hatch that
  keeps the public header narrow.
- Binary patch / diff representation. `file.modify` (spec 0011 v2)
  defines its own per-edit conflict shape inside `data_json`; a generic
  binary-diff attachment is out of scope until a real call site needs
  it.

## Acceptance Criteria

1. **Text-only round-trip.** A handler that returns
   `Output::text_only("hello")` produces a v1-compatible result through
   `Registry::dispatch`: same audit row, same `tool_after` payload's
   `output_text`, same provider-side bytes. Pinned by a regression test
   that diffs v1 vs. v2 transport bytes for every migrated built-in
   against a fixture.
2. **Structured payload visibility.** A handler that returns
   `Output{ .text = "matched 3 files", .data_json = R"([...])" }`
   reaches the Anthropic adapter as a JSON `content` array, the OpenAI
   Responses adapter as a JSON `output` field, and a fake-provider
   (spec 0017) test sink as the parsed `data_json`. The agent transcript's
   *bytes* sent to the provider differ when `data_json` is present and
   match v1 when absent.
3. **Usage propagation.** A handler that fills
   `usage = { .bytes_read = 4096, .files_touched = 1 }` produces an
   `AuditEvent` with those keys in `context`, a `ToolAfterPayload` with
   the same fields, and (once audit/log fan-out lands) a trace row
   carrying them. Slice 60 pins the hook half; audit/log fan-out remains
   downstream.
4. **Byte cap enforcement (text).** A handler that returns 257 KiB of
   `text` with the default cap produces an output whose `text.size() ≤
   256 KiB`, `usage.truncated = true`, and a single `tool_after` hook
   publish recording the truncation reason.
5. **Byte cap enforcement (data).** A handler that returns 1 MiB + 1
   bytes of serialised `data_json` produces an output whose `text` is intact,
   `data_json == std::nullopt`, and `usage.data_dropped = true`. The
   provider-adapter call still succeeds (with the text fallback).
6. **Hook redaction.** A `file.write` v2 handler's `ToolAfterPayload`
   delivered to a default-capability sink contains hashed input + byte
   counts in `usage`; the same payload delivered to a sink declaring
   `kind::trusted_local` contains the raw `data_json` field. Pinned by a
   two-sink test.
7. **Adapter mapping.** Three adapter tests (Anthropic, OpenAI
   Responses, fake) prove that `Output::text_only(...)` maps to a
   single text block in each vendor's tool-result shape, and that
   `Output{ .text, .data_json }` maps to the vendor's structured channel.
8. **Migration tax.** A v1 handler kept on `Output::text_only(...)`
   compiles, links, and passes its existing test suite **without
   modification** for at least one slice after `Output` v2 lands. The
   migration is gated, not forced.
9. **`tests/tool/test_output.cpp` ≥ 90% coverage** of the envelope (cap
   matrix × adapter matrix × audit-context matrix × hook-redaction matrix).
10. **`bench-tool` output scenarios** report envelope construction
    + serialisation cost ≤ 5 µs for `Output::text_only(...)` and ≤ 50 µs
    for a 16 KiB `data_json` payload. The latter is within spec 0002's
    ≤ 50 µs dispatch ceiling.

## Design Doc Cross-References

- [`../design-docs/tool-runtime.md`](../design-docs/tool-runtime.md) —
  "Tool Handler Shape" already documents the target envelope; the
  "Output Shape v2" section records the slice-60 public shape and points
  at this spec. The design doc owns *what the field set looks like in
  code*; this spec owns *what the user gets and how it's tested*.
- [`../design-docs/api-portability.md`](../design-docs/api-portability.md)
  — provider adapters consume `Output::data_json` per their vendor shape;
  the existing `Adapter::send` contract changes only at the
  `tool_result` rendering site.
- [`0011-file-view-and-caching.md`](0011-file-view-and-caching.md) —
  the first structured caller: `file.read` v2's
  `(start_line, end_line, fingerprint, returned_bytes, truncated)`
  tuple rides in `Output::data_json` once both specs ship. v1 of 0011
  encodes the same tuple as a text header for forward compatibility.
- [`0012-tool-scheduler-and-state.md`](0012-tool-scheduler-and-state.md)
  — the scheduler enforces the byte caps and aggregates `usage` across
  parallel calls.
- [`../rules/compile-budget.md`](../rules/compile-budget.md) — serialized
  `data_json` plus the concrete metadata-only `Attachment` keep the public
  header within budget; full JSON dependencies stay in handler/provider
  implementation TUs.

## Risks

- **JSON-in-public-header creep.** `nlohmann::json` is a heavy include.
  Mitigation: public `Output` stores serialized `data_json` bytes and no
  JSON type; full JSON parsing belongs in `.cpp` files. A
  `scripts/check-banned-includes.sh` rule (already stubbed in
  `xmake/checks.lua`) flags any `nlohmann/json.hpp` inclusion from
  `include/oran/tool/*.hpp`.
- **Adapter divergence on `data_json`.** Anthropic's tool-result
  `content` array, OpenAI Responses' `output`, and Gemini's
  `functionResponse.response` are not byte-equivalent. Mitigation: the
  agent loop sends the same `Output` value to every adapter; the
  adapter owns its serialisation. Cross-adapter A/B tests live in
  `bench/oran-provider/protocol-overhead`.
- **Spec 0011 timing dependency.** `file.read` v2's structured metadata
  is more useful with `Output` v2 shipped first. Mitigation: spec 0011
  v1 explicitly ships the same tuple as a text header so the two specs
  decouple — `Output` v2 is *required by* spec 0011 v1.1, not v1.
- **Caps surprise an agent.** A truncated `text` or dropped `data_json`
  field may break an agent that assumed full bytes. Mitigation: every
  truncation/drop is reflected in `usage`; agents that care can branch
  on the flag instead of silently ingesting a shorter blob.

## Validation

```sh
xmake build oran-tool
xmake build test-tool
xmake run test-tool "[output]"             # envelope + hook usage coverage
xmake build bench-tool
xmake run bench-tool                       # includes output.text_only / output.with_data_16kib
xmake run bench-provider protocol_overhead # planned adapter mapping A/B once oran-provider lands
```

## Out-of-Band Cross-Cuts

- `docs/design-docs/tool-runtime.md` "Output Shape v2" records the
  shipped envelope and the remaining downstream transport work.
- `docs/exec-plans/tech-debt-tracker.md` — the deep-review §`tool::Output`
  is too small P2 row closes in slice 60; the `tool::parse_input<T>` P1
  row remains open because input parsing was not part of this slice.
- `docs/design-docs/permissions-and-hooks.md` "Sink Kinds" gains a
  `kind::trusted_local` annotation when the redaction policy lands;
  this is a small edit in the same PR as v1.1.
