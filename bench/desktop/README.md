# bench-desktop

Microbenchmarks for `oran-desktop`.

## Scenarios

- `gui_compiled` — exercises the always-built build-config accessor.
- `marshal_64_text_deltas` — streamed-delta marshalling through the bounded
  UI↔runtime queue: 64 text deltas pushed through a fresh `ChatBridge` sink and
  drained into a `ChatViewModel` (the bridge's hot path). Landed with Slice C of
  [`../../docs/exec-plans/active/2026-06-14-oran-desktop-chat-tracer.md`](../../docs/exec-plans/active/2026-06-14-oran-desktop-chat-tracer.md).

## Planned

A per-delta vs. batched A-vs-B comparison once a real streaming workload exists
to tune the queue capacity / drain cadence against. Rendering itself is Slint's
concern and out of scope here (see [`../../docs/DESKTOP.md`](../../docs/DESKTOP.md)).
