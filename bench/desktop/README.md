# bench-desktop

Microbenchmarks for `oran-desktop`.

## Scenarios

- `gui_compiled` — placeholder; exercises the always-built build-config accessor.

## Planned (Slice C — bridge/view-model)

The meaningful A-vs-B comparison for this bucket is **streamed-delta marshalling
through the bounded UI↔runtime queue**: batched vs. per-delta hand-off, measuring
hand-off latency and allocations per delta. It lands with the bridge slice of
[`../../docs/exec-plans/active/2026-06-14-oran-desktop-chat-tracer.md`](../../docs/exec-plans/active/2026-06-14-oran-desktop-chat-tracer.md).
Rendering itself is Slint's concern and out of scope here (see
[`../../docs/DESKTOP.md`](../../docs/DESKTOP.md)).
