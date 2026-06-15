// include/oran/desktop/shell.hpp — desktop app window entry point.
//
// Declared unconditionally (no Slint types leak through this header, so it stays
// cheap to include and honours critical-rules.md#C6), but only *defined* when the
// binary is built with `--desktop=y` (ORAN_ENABLE_DESKTOP). Call sites guard the
// invocation on that macro and fall back to a "rebuild with --desktop=y" error
// otherwise — see `oran-bootstrap`.

#pragma once

#include <oran/core/result.hpp>

namespace orangutan::desktop::shell {

/// Options for the desktop window run. Slice A (skeleton window) takes none; the
/// chat tracer slice adds the prompt runner, runtime executor, and the
/// config-derived UI preferences (theme / reduce-motion).
struct RunOptions {};

/// Open the desktop window and drive the Slint event loop until the window is
/// closed, returning the process exit code. Blocks the calling thread for the
/// lifetime of the window (Slint owns the UI thread; the agent runtime is
/// bridged onto its own executor in a later slice — see `docs/DESKTOP.md`).
[[nodiscard]] core::Result<int> run(RunOptions options = {});

}  // namespace orangutan::desktop::shell
