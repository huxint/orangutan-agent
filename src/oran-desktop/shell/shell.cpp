// src/oran-desktop/shell/shell.cpp — Slint chat window entry (built only with --desktop=y).
//
// `app_window.h` is generated from `ui/app_window.slint` by the `slint-compiler`
// invocation wired into the `oran-desktop-shell` target's `before_build` step;
// the generated directory is on this library's include path. The generated code
// is confined to this library (module-and-pch / compile-budget rules).
//
// This is the UI half of the bridge seam (docs/DESKTOP.md). It owns no agent
// logic: the `send` / `stop` callbacks poke the `ChatBridge`, and a repeating
// Slint timer drains the bridge's runtime->UI queue into a `ChatViewModel` on
// the UI thread, then mirrors that state into the Slint properties. The
// thread crossing is the bridge's lock-guarded channels (`drain` uses
// `try_receive`), so the timer never touches the runtime coroutine directly.

#include <oran/desktop/shell.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <slint.h>

#include <app_window.h>

#include <oran/desktop/chat_bridge.hpp>

namespace orangutan::desktop::shell {
namespace {

std::string status_line(const ChatViewModel& view_model) {
  switch (view_model.status()) {
    case TurnStatus::idle:
    case TurnStatus::done:
      return "Ready.";
    case TurnStatus::streaming:
      return "Streaming… — press Stop to cancel";
    case TurnStatus::error:
      return "Error: " + std::string{view_model.error_message()};
  }
  return "Ready.";
}

// Mirror the folded view-model into the Slint properties. Rebuilds the whole
// line model each refresh — fine for a chat tracer's transcript sizes.
void refresh(AppWindow& window, const ChatViewModel& view_model) {
  std::vector<ChatLineData> rows;
  rows.reserve(view_model.lines().size());
  for (const auto& line : view_model.lines()) {
    rows.push_back(ChatLineData{
        .is_user = line.role == ChatLine::Role::user,
        .text = slint::SharedString{std::string_view{line.text}},
    });
  }
  window.set_lines(std::make_shared<slint::VectorModel<ChatLineData>>(std::move(rows)));
  window.set_status_text(slint::SharedString{std::string_view{status_line(view_model)}});
  window.set_streaming(view_model.status() == TurnStatus::streaming);
}

}  // namespace

core::Result<int> run(RunOptions options) {
  auto window = AppWindow::create();
  window->set_dark(options.dark);

  // UI-thread transcript state. `submit` opens the user+assistant lines; the
  // drain timer folds streamed deltas into the same model. Held by shared_ptr
  // so the Slint-owned callbacks keep it alive for the window's lifetime.
  auto view_model = std::make_shared<ChatViewModel>();
  auto* bridge = options.bridge;
  const slint::ComponentWeakHandle<AppWindow> weak{window};

  window->on_send([weak, bridge, view_model]() {
    auto strong = weak.lock();
    if (!strong) {
      return;
    }
    auto& window_ref = **strong;
    auto prompt = std::string{std::string_view{window_ref.get_input_text()}};
    if (prompt.empty() || bridge == nullptr) {
      return;
    }
    window_ref.set_input_text(slint::SharedString{});
    view_model->submit_user(prompt);
    // Overflow/closed is surfaced by the next turn's absence; the bridge's
    // bounded prompt queue is far larger than an operator can outpace.
    static_cast<void>(bridge->submit(std::move(prompt)));
    refresh(window_ref, *view_model);
  });

  window->on_stop([bridge]() {
    if (bridge != nullptr) {
      bridge->request_stop();
    }
  });

  // Drain the runtime->UI queue on a cadence; refresh only when something
  // arrived so idle frames stay cheap.
  slint::Timer drain_timer;
  if (bridge != nullptr) {
    drain_timer.start(slint::TimerMode::Repeated, std::chrono::milliseconds{33}, [weak, bridge, view_model]() {
      auto strong = weak.lock();
      if (!strong) {
        return;
      }
      if (bridge->drain(*view_model) > 0) {
        refresh(**strong, *view_model);
      }
    });
  }

  window->run();

  // The operator closed the window: close the bridge's input so the runtime's
  // `run_chat_session` winds down (the launcher then stops the runtime).
  if (bridge != nullptr) {
    bridge->close();
  }
  return 0;
}

}  // namespace orangutan::desktop::shell
