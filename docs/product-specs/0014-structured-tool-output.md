# 0014 — Structured Tool Output (`ToolOutput` v2)

## User Problem

`tool::Output` used to be `{ std::string text; bool is_error; }`.
Slice 60 promotes it into a forward-compatible envelope in
[`include/oran/tool/output.hpp`](../../include/oran/tool/output.hpp);
the remaining problem is that most built-ins and every future provider
adapter still consume only the text fallback. Current built-ins still
flatten structured facts into prose:

- `FileRead` returns the file body as text; range metadata, fingerprint,
  and truncation flag (spec 0011) have nowhere to live except a
  hand-rolled header line.
- `FileSearch` returns one `<path>:<line>: <preview>` line per match; the
  match count, byte budget consumed, and per-file fingerprints are
  invisible.
- The future `CodeOutline` / `CodeSymbols` (per
  [`../design-docs/tool-runtime.md`](../design-docs/tool-runtime.md)
  "Built-in Tool Categories" and the deep-review §Tool design judgment)
  needs to return a structured symbol tree — converting it to markdown in
  the handler then reparsing it in the agent is a downstream-of-the-LLM
  anti-pattern.
- `MemoryRecall` distinguishes *the records retrieved* from *the summary
  surfaced to the model* by returning model-facing recall text plus serialized
  `data_json` record metadata.

The cost is paid at four call sites:

1. **Provider adapters** re-parse strings to derive structure they need
   for vendor tool-result blocks.
2. **The desktop app and channel adapters** lose the ability to render
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

> **Status (slice 107, 2026-05-26):** the base envelope ships in
> `oran-tool`. `Output::text_only(...)` preserves the existing text path,
> `Output::error(...)` marks structured-capable errors, `ToolAfterPayload`
> copies `Output::usage` on successful dispatch, and slice 65 copies
> successful `Output::data_json` into `ToolAfterPayload::data_json` while
> `hook::Bus` redacts it for every sink except `SinkKind::trusted_local`.
> Slice 66 adds the shared cap primitive:
> `OutputCapOptions`, `OutputCapReport`, and `apply_output_caps` live in
> `<oran/tool/output.hpp>`, `Registry::dispatch` applies
> `DispatchContext::output_caps` to successful handler output before it is
> returned or published to `tool_after`, and `oran-config` parses the
> documented `runtime.tool_output.max_text_bytes` /
> `runtime.tool_output.max_data_bytes` defaults for the future scheduler /
> agent owner to thread into that context.
> Slice 67 adds audit usage fan-out for the same direct-dispatch boundary:
> `Registry::dispatch` records the permission decision row before the handler
> runs, then after a successful handler return and cap application it serializes
> non-empty `Output::usage` under `metadata_json.usage` and best-effort updates
> that same row through `AuditSink::update_metadata(...)` /
> `AuditRepository::update_event_metadata(...)`. This preserves the one-row
> permission-decision invariant while making bytes, touched-file counts,
> match counts, wall time, and cap flags queryable from the audit log.
> `FileRead` carries its
> requested text plus range/fingerprint tuple in serialized `data_json`
> while keeping the spec-0011 text fallback, the current mutation built-ins
> fill measured usage counters while keeping `data_json=nullopt` for the v1
> migration path, `FileSearch` fills serialized `data_json` with
> `{kind:"file_search", path, pattern, regex, matches[], match_count,
> truncated, truncation_reason, files_scanned, bytes_read}` plus usage
> `bytes_read` / `files_touched` / `match_count` / `truncated`, and
> `DirectoryList` fills serialized `data_json` with
> `{kind:"directory_list", path, include_hidden, max_entries, entry_count,
> entries[]}` plus usage `files_touched=1` and `match_count=entry_count`.
> Slice 168 adds `MemoryRecall`, which fills `data_json` with
> `{kind:"memory_recall", match_count, records[]}` plus
> `usage.match_count`, while keeping deterministic recall text as the
> provider fallback.
> Slice 169 adds `MemoryRemember`, which fills `data_json` with
> `{kind:"memory_remember", record:{...}}` plus `usage.bytes_written`, while
> keeping a compact confirmation text as the provider fallback.
> Slice 170 adds `MemoryForget`, which fills `data_json` with
> `{kind:"memory_forget", record:{id, scope_key}}` plus
> `usage.bytes_written = 0`, while keeping compact confirmation text as the
> provider fallback.
> With slice 170 the shipped built-in side of spec 0014's structured-output
> migration is current: every shipped filesystem built-in fills usage counters,
> every shipped read-side built-in fills `data_json`, and the shipped
> memory mutation built-ins follow the same structured envelope. `bench-tool`
> has `output.text_only` vs. `output.with_data_16kib` plus
> `output.apply_caps` coverage. Slice 107 closes the first provider-facing
> mapping step: `core::ToolResultContent` now preserves optional
> `data_json`, `agent::Loop` copies successful `tool::Output::data_json`
> into provider-facing tool-result blocks, and
> `provider::make_protocol_request` maps those bytes into serialized OpenAI
> Responses `function_call_output.output`, while Anthropic Messages keeps
> provider-compatible text `tool_result.content` fallback.
> Response decoding and the first injected transport-backed Anthropic/OpenAI
> factories now land in slices 108-109, slice 110 adds the `oran-http`
> body client, slice 111 adds the bootstrap-owned
> `http::Client`-backed `ProtocolTransport` adapter, and slice 112 wires that
> backend into configured-route `bootstrap::run`. SSE transport and
> Gemini/custom mappings remain downstream.

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
  - Anthropic Messages tool-result: the adapter sends `text` as provider-
    compatible content when present, or serialized `data_json` bytes as plain
    text only when no fallback text exists.
  - OpenAI Responses tool-result: `data_json` is serialised as JSON in the
    `output` field; otherwise the rendered `text`.
  - Gemini, custom-OpenAI-compatible: same pattern — JSON when
    `data_json.has_value()`, plain text otherwise.
  - Adapters never *invent* structure: the agent loop, not the adapter,
    decides what to send.
- **Audit fan-out.** `permission::AuditEvent` already carries
  `metadata_json`; slice 67 records the permission decision row before any
  handler side effects, then updates that same row after a successful handler
  result and cap application when `Output::usage` is non-empty. The dispatch
  pipeline writes a `usage` object containing the measured keys
  (`bytes_read`, `bytes_written`, `files_touched`, `match_count`,
  `cost_estimate`, `wall_time_ms`) and the cap flags (`truncated`,
  `data_dropped`) when present. The existing `input_hash` discipline is
  unchanged, and enrichment never appends a second audit decision row.
- **Hook fan-out.** `ToolAfterPayload` (already defined in slice 22 +
  slice 25) now has a `usage` field carrying the same metrics and, as of
  slice 65, optional `data_json` carrying the raw serialized structured output
  bytes from successful tool dispatch. The payload's `output_text` stays the
  truncated rendering used today. `hook::Bus` builds at most one raw payload
  snapshot and one default/redacted snapshot per publish, clearing `data_json`
  in the default view unless the consuming sink's `kind()` returns
  `SinkKind::trusted_local`, matching the deep-review §Hook/audit redaction
  recommendation. Slice 152 extends that same trust boundary to sensitive
  mutation inputs: `FileWrite` / `FileEdit` lifecycle payloads carry a
  `redacted_input_json` summary, and `hook::Bus` substitutes it in the shared
  default view while trusted-local sinks keep the raw input.
- **Byte caps**. Two caps, independent:
  - `runtime.tool_output.max_text_bytes` (default 256 KiB) — applies to
    `text`. Exceeding it truncates and sets `is_error=false` with a
    `truncated=true` flag captured in `usage`.
  - `runtime.tool_output.max_data_bytes` (default 1 MiB) — applies to
    serialised `data_json`. Exceeding it drops `data_json`, leaves `text` intact,
    and records `data_dropped=true` in `usage`.
  Caps fire at the dispatch/scheduler boundary, not in each handler. Until
  the scheduler exists, `Registry::dispatch` applies the same helper after a
  successful handler return; when `oran-agent` lands, the scheduler owns the
  options and calls the shared primitive before returning ordered results.
- **Migration path.** Built-ins migrate one at a time:
  1. `FileRead` — shipped in slice 62: keeps the text header/body fallback
     and fills `data_json` with `{kind:"file_read", path, text, fingerprint,
     start_line, end_line, returned_bytes, truncated}` plus
     `usage.bytes_read`, `files_touched`, and `truncated`.
  2. `FileSearch` — shipped in slice 63: keeps the existing
     `path:line:text` text rendering and trailing truncation summary, and
     fills `data_json` with `{kind:"file_search", path, pattern, regex,
     matches[], match_count, truncated, truncation_reason, files_scanned,
     bytes_read}` plus `usage.bytes_read` (cumulative scanned file bytes),
     `files_touched` (non-binary scanned files), `match_count`
     (post-truncation), and the `truncated` cap flag.
  3. `DirectoryList` — shipped in slice 64: keeps the existing
     `<path>:<kind>:<size_bytes or '-'>` text rendering, and fills
     `data_json` with `{kind:"directory_list", path, include_hidden,
     max_entries, entry_count, entries[]}` where each entry is
     `{name, path, kind, size_bytes}` (JSON null `size_bytes` for
     non-regular kinds) plus `usage.files_touched=1` and
     `usage.match_count=entry_count`.
  4. `FileWrite` / `FileEdit` / `FileDelete` — shipped in slice 61:
     `FileWrite` fills `usage.bytes_written` and `files_touched`;
     `FileEdit` fills `bytes_read`, `bytes_written`, `files_touched`,
     and `match_count`; `FileDelete` fills `bytes_written=0` and
     `files_touched=1`; `data_json` stays `nullopt` for v1.
  5. `MemoryRecall` — shipped in slice 168: keeps a deterministic text
     fallback (`MemoryRecall: <n> match(es)` plus recall framing), fills
     `data_json` with `{kind:"memory_recall", match_count, records[]}`, and
     fills `usage.match_count`.
  6. `MemoryRemember` — shipped in slice 169: keeps a compact confirmation
     fallback (`MemoryRemember: saved <kind> record <id>`), fills `data_json`
     with `{kind:"memory_remember", record:{...}}`, and fills
     `usage.bytes_written` from the saved record payload.
  7. `MemoryForget` — shipped in slice 170: keeps a compact confirmation
     fallback (`MemoryForget: removed record <id>`), fills `data_json` with
     `{kind:"memory_forget", record:{id, scope_key}}`, and fills
     `usage.bytes_written = 0`.
  All built-ins shipped to date have completed their v1 migration to the
  structured envelope; new built-ins ship with usage counters and
  `data_json` from the start.

## Scope (v1.1)

- **`Attachment` shape finalised** when the first tool produces one
  (likely a `FileRead` binary fallback or a `CodeOutline` rendered
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

- Streaming tool output. Long-running tools (`ShellExec`, future
  `code.test`) emit chunks via a `tool::OutputSink` analogous to the
  provider streaming sink in
  [`../design-docs/api-portability.md`](../design-docs/api-portability.md).
  Closes when the agent loop's streaming path is mature enough that
  partial outputs don't break transcript ordering.
- Multi-language attachment rendering for the desktop app (spec 0007).

## Out Of Scope

- A typed-variant alternative (`std::variant<TextOutput, FileReadOutput,
  SearchOutput, ...>`). Considered; rejected because every new tool
  would force a recompile of every consumer, breaking the compile
  budget. The serialized `data_json` channel is the escape hatch that
  keeps the public header narrow.
- Binary patch / diff representation. `FileModify` (spec 0011 v2)
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
   reaches the OpenAI Responses adapter as a JSON `output` string and a
   fake-provider (spec 0017) test sink as the preserved `data_json`; Anthropic
   Messages receives the text fallback so arbitrary local JSON objects are not
   placed in `tool_result.content`. Slice 107 ships this for the offline
   request-serialization boundary: the agent transcript keeps both `output`
   and `data_json`, protocol request tests prove text-only and structured
   tool-result shapes where supported, and malformed structured bytes fail
   before transport for protocols that parse them. Response decoding and
   HTTP-backed adapter factories remain downstream.
3. **Usage propagation.** Shipped for direct dispatch in slice 67. A handler that fills
   `usage = { .bytes_read = 4096, .files_touched = 1 }` produces an
   audit row whose `metadata_json.usage` carries those keys, a
   `ToolAfterPayload` with the same fields, and the same cap flags after
   `apply_output_caps` runs. Slice 60 pinned the hook half; slice 67 pins
   the audit half without changing the pre-handler decision-recording
   invariant. A later trace/log sink can read the same `ToolUsage` fields
   when the observability layer lands.
4. **Byte cap enforcement (text).** Shipped in slice 66. A handler that returns 257 KiB of
   `text` with the default cap produces an output whose `text.size() ≤
   256 KiB`, `usage.truncated = true`, and a single `tool_after` hook
   publish carrying the capped text plus `usage.truncated`.
5. **Byte cap enforcement (data).** Shipped in slice 66. A handler that returns 1 MiB + 1
   bytes of serialised `data_json` produces an output whose `text` is intact,
   `data_json == std::nullopt`, and `usage.data_dropped = true`. The
   future provider-adapter call still succeeds (with the text fallback).
6. **Hook redaction.** Shipped in slice 65 for structured output and slice
   152 for sensitive mutation inputs. A structured-output handler's
   `ToolAfterPayload` delivered to a default sink contains the text fallback
   and byte/count metrics in `usage` but no raw `data_json`; the same payload
   delivered to a sink whose `kind()` returns `SinkKind::trusted_local`
   contains the raw `data_json` field. For `FileWrite` / `FileEdit`,
   default sinks receive a redacted `input_json` summary containing the full
   input hash plus byte counts, while trusted-local sinks receive the original
   mutation input. Pinned by hook-bus and registry two-sink tests.
7. **Adapter mapping.** Slice 107 ships the first request-side mapping
   coverage: Anthropic Messages and OpenAI Responses tests prove that
   text-only tool results use the plain fallback and structured
   `ToolResultContent::data_json` maps to each protocol's structured
   tool-result field; a fake-provider loop test proves `tool::Output` reaches
   the provider-facing transcript without losing `data_json`. Slices 108-109
   add response decoding and an injected body-response transport factory seam,
   slice 110 adds the `oran-http` body client, slice 111 adds the
   bootstrap-owned `http::Client`-backed `ProtocolTransport` adapter, and
   slice 112 wires that backend into configured-route `bootstrap::run`. SSE
   transport and non-Anthropic/OpenAI protocol families remain follow-up work.
8. **Migration tax.** A v1 handler kept on `Output::text_only(...)`
   compiles, links, and passes its existing test suite **without
   modification** for at least one slice after `Output` v2 lands. The
   migration is gated, not forced.
9. **`tests/tool/test_output.cpp` ≥ 90% coverage** of the envelope (cap
   matrix × adapter matrix × audit-context matrix × hook-redaction matrix).
10. **`bench-tool` output scenarios** report envelope construction
    + serialisation cost ≤ 5 µs for `Output::text_only(...)`, ≤ 50 µs
    for a 16 KiB `data_json` payload, and the cap helper cost for a
    representative oversized payload. The structured-output path remains
    within spec 0002's ≤ 50 µs dispatch ceiling.

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
  the first structured caller: `FileRead` v2's
  `(path, text, start_line, end_line, fingerprint, returned_bytes,
  truncated)` payload now rides in `Output::data_json`, while v1 of 0011
  keeps the same facts as a text header for forward compatibility.
- [`0012-tool-scheduler-and-state.md`](0012-tool-scheduler-and-state.md)
  — slice 66 provides the shared byte-cap helper and direct dispatch applies
  it for pre-scheduler callers; the scheduler owns the same cap options and
  aggregates `usage` across parallel calls once batched tool calls land.
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
- **Adapter divergence on `data_json`.** Anthropic Messages text
  `tool_result.content`, OpenAI Responses' JSON-string `output`, and Gemini's
  `functionResponse.response` are not byte-equivalent. Mitigation: the
  agent loop sends the same `Output` value to every adapter; the
  adapter owns its serialisation. Cross-adapter A/B tests will extend
  `bench-provider` once the first protocol mapper lands.
- **Spec 0011 timing dependency.** `FileRead` v2's structured metadata
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
xmake run test-tool "[file_write],[file_edit],[file_delete]"
xmake build bench-tool
xmake run bench-tool                       # includes output.text_only / output.with_data_16kib
xmake run bench-provider                  # cache-hint mapping today; protocol mapping A/B later
```

## Out-of-Band Cross-Cuts

- `docs/design-docs/tool-runtime.md` "Output Shape v2" records the
  shipped envelope and the remaining downstream transport work.
- `docs/exec-plans/tech-debt-tracker.md` — the deep-review §`tool::Output`
  is too small P2 row closes in slice 60; the `tool::parse_input<T>` P1
  row remains open because input parsing was not part of this slice.
- `docs/design-docs/permissions-and-hooks.md` "Sink Kinds" records the
  shipped `SinkKind::trusted_local` annotation and the bus-enforced
  `ToolAfterPayload::data_json` redaction policy.
