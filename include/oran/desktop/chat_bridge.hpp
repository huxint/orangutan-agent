// include/oran/desktop/chat_bridge.hpp — always-built desktop bridge surface.
//
// The Slint↔runtime bridge for the chat tracer
// (docs/exec-plans/active/2026-06-14-oran-desktop-chat-tracer.md, Slice C). This
// header carries only pure-C++ types — no Slint include (critical-rules.md#C6) —
// so the whole bridge compiles and unit-tests in the default build:
//
//   * `UiUpdate`         — one runtime→UI streamed event.
//   * `ChatViewModel`    — renderable chat state the Slint view binds to.
//   * `DesktopEventSink` — a `provider::EventSink` that turns streamed deltas
//                          into `UiUpdate`s and hands them to a delivery hook.
//   * `ChatBridge`       — bounded UI↔runtime queues + per-turn cancellation,
//                          wiring the sink's delivery into the runtime→UI queue.
//
// The Slint shell (Slice D) supplies the delivery hook that marshals updates
// onto the UI thread via Slint's event-loop post API and drains them into a
// `ChatViewModel`; tests drive the same surface with a fake provider.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <asio/any_io_executor.hpp>
#include <asio/cancellation_signal.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/async/channel.hpp>
#include <oran/core/result.hpp>
#include <oran/core/stop_reason.hpp>
#include <oran/provider/system.hpp>

namespace orangutan::desktop {

/// The kind of one runtime→UI streamed event. Each value corresponds to a
/// `provider::EventSink` callback the desktop renders, plus a terminal `error`
/// the bridge synthesises when a turn fails.
enum class UiUpdateKind : std::uint8_t {
  text_delta,
  thinking_delta,
  tool_start,
  done,
  error,
};

/// One streamed event flowing from the runtime to the UI. `text` carries the
/// answer/thinking delta, the tool name (`tool_start`), or the error message
/// (`error`); `tool_id` is the vendor tool-use id for `tool_start`;
/// `stop_reason` is meaningful only for `done`.
struct UiUpdate {
  UiUpdateKind kind{UiUpdateKind::text_delta};
  std::string text{};
  std::string tool_id{};
  core::StopReason stop_reason{core::StopReason::end_turn};

  friend bool operator==(const UiUpdate&, const UiUpdate&) = default;
};

/// Lifecycle of the current chat turn as the view sees it.
enum class TurnStatus : std::uint8_t {
  idle,       ///< No turn has run yet (or the model was reset).
  streaming,  ///< A prompt was submitted; deltas may still arrive.
  done,       ///< The turn completed normally (`on_done`).
  error,      ///< The turn ended in an error update.
};

/// One rendered transcript line. The chat tracer only distinguishes operator
/// prompts from assistant answers; tool output and thinking are surfaced
/// through `ChatViewModel` side-channels, not as transcript lines.
struct ChatLine {
  enum class Role : std::uint8_t {
    user,
    assistant
  };

  Role role{Role::user};
  std::string text{};

  friend bool operator==(const ChatLine&, const ChatLine&) = default;
};

/// Renderable chat state. Pure value logic with no Slint or asio dependency so
/// the Slint view (Slice D) and tests fold the same `UiUpdate` stream into the
/// same observable state.
///
/// Usage: `submit_user` opens a streaming turn (an operator line plus an empty
/// assistant line); each `apply(UiUpdate)` folds one streamed event into the
/// open turn. The view reads `lines()`, `streaming_text()`, `thinking_text()`,
/// `tool_calls()`, `status()`, and `error_message()`.
class ChatViewModel {
public:
  /// Append the operator's `prompt` and open a fresh assistant line, clearing
  /// the previous turn's thinking / tool / error side-channels. Status becomes
  /// `streaming`.
  void submit_user(std::string prompt);

  /// Fold one streamed event into the open turn.
  void apply(const UiUpdate& update);

  [[nodiscard]] const std::vector<ChatLine>& lines() const noexcept {
    return lines_;
  }

  /// The in-progress assistant answer text (the open assistant line), or empty
  /// when no turn has started.
  [[nodiscard]] std::string_view streaming_text() const noexcept;

  /// Accumulated extended-thinking text for the current turn.
  [[nodiscard]] std::string_view thinking_text() const noexcept {
    return thinking_;
  }

  /// Tool names started during the current turn, in order.
  [[nodiscard]] const std::vector<std::string>& tool_calls() const noexcept {
    return tool_calls_;
  }

  [[nodiscard]] TurnStatus status() const noexcept {
    return status_;
  }

  /// The message from the last `error` update, empty when the turn did not
  /// error.
  [[nodiscard]] std::string_view error_message() const noexcept {
    return error_;
  }

private:
  std::vector<ChatLine> lines_;
  std::string thinking_;
  std::vector<std::string> tool_calls_;
  std::string error_;
  TurnStatus status_{TurnStatus::idle};
};

/// `provider::EventSink` that converts streamed deltas into `UiUpdate`s and
/// hands each to a delivery hook. The hook is the thread-marshalling seam: the
/// Slint shell (Slice D) posts onto the UI event loop, while tests deliver
/// synchronously into a vector or a `ChatViewModel`. The sink itself stays
/// Slint-free and runs on whichever coroutine drives `provider::System::send`,
/// honouring the `EventSink` "not thread-safe; hop your own strand" contract.
///
/// `on_tool_delta` (partial JSON args) is intentionally left as the base
/// no-op: the chat tracer renders a tool's start, not its streaming arguments.
class DesktopEventSink final : public provider::EventSink {
public:
  /// Receives one translated `UiUpdate`. Invoked once per non-empty streamed
  /// callback, in arrival order.
  using Delivery = std::function<void(const UiUpdate&)>;

  explicit DesktopEventSink(Delivery deliver);

  void on_text_delta(std::string_view delta) override;
  void on_thinking_delta(std::string_view delta) override;
  void on_tool_start(std::string_view id, std::string_view name) override;
  void on_done(core::StopReason stop_reason) override;

  /// Count of `UiUpdate`s handed to the delivery hook.
  [[nodiscard]] std::size_t updates_delivered() const noexcept {
    return delivered_;
  }

private:
  void deliver(UiUpdate update);

  Delivery deliver_;
  std::size_t delivered_{0};
};

/// Construction inputs for `ChatBridge`. `executor` is the shared
/// `async::Runtime` executor both channels post on. Capacities bound the two
/// queues (async-and-concurrency.md A5): `prompt_capacity` is small (the
/// operator submits one prompt at a time), `update_capacity` larger to absorb
/// a burst of streamed deltas before the UI thread drains them.
struct ChatBridgeOptions {
  asio::any_io_executor executor;
  std::size_t prompt_capacity{8};
  std::size_t update_capacity{256};
};

/// The Slint↔runtime bridge: bounded UI↔runtime queues plus per-turn
/// cancellation, with a `DesktopEventSink` whose delivery enqueues onto the
/// runtime→UI queue.
///
/// Threading. The UI thread calls `submit`, `request_stop`, and `drain`; the
/// runtime coroutine calls `next_prompt` and passes `event_sink()` to
/// `provider::System::send`. The crossing is safe because both directions use
/// `async::Channel`'s lock-guarded `try_send` / `try_receive`; the runtime side
/// `co_await`s `next_prompt()`. Updates that arrive while the runtime→UI queue
/// is full are dropped and counted (`updates_dropped`) rather than blocking the
/// provider coroutine — a deliberate backpressure choice for a live tracer.
///
/// Cancellation. `request_stop` emits a terminal signal; a turn whose awaitable
/// is bound to `cancellation_slot()` observes `Error::cancelled`. The single
/// signal models one in-flight turn; per-turn signal lifecycle is the shell's
/// concern (Slice D).
class ChatBridge {
public:
  explicit ChatBridge(ChatBridgeOptions options);

  ChatBridge(const ChatBridge&) = delete;
  ChatBridge& operator=(const ChatBridge&) = delete;
  ChatBridge(ChatBridge&&) = delete;
  ChatBridge& operator=(ChatBridge&&) = delete;

  ~ChatBridge() = default;

  // UI thread → runtime.

  /// Enqueue an operator prompt. Returns `mailbox_overflowed` if the prompt
  /// queue is full, `cancelled` if the bridge is closed.
  [[nodiscard]] core::Result<void> submit(std::string prompt);

  /// Emit a terminal cancellation on the in-flight turn's signal.
  void request_stop() noexcept;

  /// Slot to bind onto the spawned turn so `request_stop` cancels it.
  [[nodiscard]] asio::cancellation_slot cancellation_slot() noexcept;

  // Runtime side.

  /// Await the next submitted prompt. Cancel-aware via `cancellation_slot()`.
  [[nodiscard]] async::Awaitable<core::Result<std::string>> next_prompt();

  /// The sink to hand `provider::System::send`; its deltas flow to the
  /// runtime→UI queue.
  [[nodiscard]] provider::EventSink* event_sink() noexcept {
    return &sink_;
  }

  // UI thread ← runtime.

  /// Drain all buffered updates into `view_model`, returning the count applied.
  std::size_t drain(ChatViewModel& view_model);

  /// Count of updates dropped because the runtime→UI queue was full.
  [[nodiscard]] std::size_t updates_dropped() const noexcept {
    return dropped_;
  }

  /// Close both queues; pending and future sends/receives fail with `cancelled`.
  void close() noexcept;

private:
  async::Channel<std::string> prompts_;
  async::Channel<UiUpdate> updates_;
  asio::cancellation_signal cancel_;
  std::size_t dropped_{0};
  DesktopEventSink sink_;
};

}  // namespace orangutan::desktop
