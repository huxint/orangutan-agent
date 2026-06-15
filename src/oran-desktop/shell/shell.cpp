// src/oran-desktop/shell/shell.cpp — Slint window entry (built only with --desktop=y).
//
// `app_window.h` is generated from `ui/app_window.slint` by the `slint-compiler`
// invocation wired into the `oran-desktop` target's `before_build` step; the
// generated directory is on this library's include path. The generated code is
// confined to this library (module-and-pch / compile-budget rules).

#include <oran/desktop/shell.hpp>

#include <app_window.h>

namespace orangutan::desktop::shell {

core::Result<int> run(RunOptions /*options*/) {
  // Create the window component and spin the Slint event loop until it closes.
  // Slint's C++ API does not throw across this boundary; the chat tracer slice
  // replaces this with the runtime-bridged view (prompt submit + streamed
  // deltas + stop), driven through `provider::EventSink`.
  auto window = AppWindow::create();
  window->run();
  return 0;
}

}  // namespace orangutan::desktop::shell
