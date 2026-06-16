// include/oran/desktop/shell.hpp — desktop app window entry point.
//
// Declared unconditionally (no Slint types leak through this header, so it stays
// cheap to include and honours critical-rules.md#C6), but only *defined* when the
// binary is built with `--desktop=y` (ORAN_ENABLE_DESKTOP). Call sites guard the
// invocation on that macro and fall back to a "rebuild with --desktop=y" error
// otherwise — see `oran-bootstrap`.

#pragma once

#include <oran/core/result.hpp>

namespace orangutan::desktop {
class ChatBridge;
}  // namespace orangutan::desktop

namespace orangutan::desktop::shell {

/// Inputs for the desktop window run. The window binds to a `ChatBridge` the
/// caller already wired to a runtime-side `run_chat_session`: the UI raises
/// `submit` / `request_stop` on it and a drain timer folds its streamed updates
/// into the transcript. `bridge` is borrowed for the window's lifetime; the UI
/// preferences come from `DesktopConfig` (theme / reduce-motion).
struct RunOptions {
  ChatBridge* bridge{nullptr};
  bool dark{false};
  bool reduce_motion{false};
};

/// Open the desktop window and drive the Slint event loop until the window is
/// closed, returning the process exit code. Blocks the calling thread for the
/// lifetime of the window (Slint owns the UI thread; the agent runtime runs on
/// its own `async::Runtime` workers via `Runtime::start()`, bridged through
/// `options.bridge` — see `docs/DESKTOP.md`). On close the bridge's input is
/// closed so the session loop winds down. A null `bridge` opens an inert window.
[[nodiscard]] core::Result<int> run(RunOptions options = {});

}  // namespace orangutan::desktop::shell
