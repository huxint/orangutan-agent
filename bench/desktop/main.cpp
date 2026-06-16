// bench/desktop/main.cpp — registers and runs oran-desktop nanobench scenarios.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <cstddef>

#include <asio/io_context.hpp>

#include <oran/desktop/chat_bridge.hpp>
#include <oran/desktop/desktop.hpp>

namespace {

namespace desktop = orangutan::desktop;

// Push `count` text deltas through a fresh bridge sink (the runtime side) and
// drain them into a view-model (the UI side). This is the bridge's hot path:
// the bounded runtime->UI queue hand-off plus the view-model fold, with Slint
// rendering deliberately excluded (Slint owns pixels — see docs/DESKTOP.md).
void marshal_deltas(const asio::any_io_executor& executor, std::size_t count) {
  desktop::ChatBridge bridge{desktop::ChatBridgeOptions{.executor = executor, .update_capacity = count + 1}};
  desktop::ChatViewModel view_model;
  view_model.submit_user("bench");

  auto* sink = bridge.event_sink();
  for (std::size_t i = 0; i < count; ++i) {
    sink->on_text_delta("tok ");
  }
  ankerl::nanobench::doNotOptimizeAway(bridge.drain(view_model));
}

}  // namespace

int main() {
  ankerl::nanobench::Bench b;
  b.title("bench-desktop");

  asio::io_context io;
  const auto executor = io.get_executor();

  b.run("gui_compiled", [] { ankerl::nanobench::doNotOptimizeAway(orangutan::desktop::gui_compiled()); });

  // Streamed-delta marshalling through the bounded UI<->runtime queue.
  b.run("marshal_64_text_deltas", [&] { marshal_deltas(executor, 64); });

  return 0;
}
