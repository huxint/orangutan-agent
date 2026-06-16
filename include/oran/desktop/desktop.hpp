// include/oran/desktop/desktop.hpp — oran-desktop library surface.
//
// The bridge / view-model layer (bounded UI<->runtime queues, the
// `provider::EventSink` that marshals streamed deltas onto the Slint UI thread,
// and the chat view-model) lives in `<oran/desktop/chat_bridge.hpp>` (Slice C of
// `docs/exec-plans/active/2026-06-14-oran-desktop-chat-tracer.md`). This header
// carries the build-config accessor plus the gated window entry
// (`<oran/desktop/shell.hpp>`).

#pragma once

namespace orangutan::desktop {

/// True when the binary was built with the Slint UI shell (`--desktop=y`,
/// which defines `ORAN_ENABLE_DESKTOP`). Lets `oran-bootstrap` and tests reason
/// about whether `shell::run` has a definition without referencing any Slint
/// type, keeping this header free of the heavy GUI include (critical-rules.md#C6).
[[nodiscard]] bool gui_compiled() noexcept;

}  // namespace orangutan::desktop
