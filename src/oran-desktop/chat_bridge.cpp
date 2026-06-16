// src/oran-desktop/chat_bridge.cpp — always-built desktop bridge implementation.

#include <oran/desktop/chat_bridge.hpp>

#include <utility>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_type.hpp>
#include <asio/co_spawn.hpp>
#include <asio/post.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

namespace orangutan::desktop {
void ChatViewModel::submit_user(std::string prompt) {
  lines_.push_back(ChatLine{.role = ChatLine::Role::user, .text = std::move(prompt)});
  lines_.push_back(ChatLine{.role = ChatLine::Role::assistant, .text = {}});
  thinking_.clear();
  tool_calls_.clear();
  error_.clear();
  status_ = TurnStatus::streaming;
}

void ChatViewModel::apply(const UiUpdate& update) {
  switch (update.kind) {
    case UiUpdateKind::text_delta:
      if (!lines_.empty()) {
        lines_.back().text += update.text;
      }
      break;
    case UiUpdateKind::thinking_delta:
      thinking_ += update.text;
      break;
    case UiUpdateKind::tool_start:
      tool_calls_.push_back(update.text);
      break;
    case UiUpdateKind::done:
      status_ = TurnStatus::done;
      break;
    case UiUpdateKind::error:
      error_ = update.text;
      status_ = TurnStatus::error;
      break;
  }
}

std::string_view ChatViewModel::streaming_text() const noexcept {
  for (auto it = lines_.rbegin(); it != lines_.rend(); ++it) {
    if (it->role == ChatLine::Role::assistant) {
      return it->text;
    }
  }
  return {};
}

DesktopEventSink::DesktopEventSink(Delivery deliver) : deliver_{std::move(deliver)} {}

void DesktopEventSink::on_text_delta(std::string_view delta) {
  deliver(UiUpdate{.kind = UiUpdateKind::text_delta, .text = std::string{delta}});
}

void DesktopEventSink::on_thinking_delta(std::string_view delta) {
  deliver(UiUpdate{.kind = UiUpdateKind::thinking_delta, .text = std::string{delta}});
}

void DesktopEventSink::on_tool_start(std::string_view id, std::string_view name) {
  deliver(UiUpdate{.kind = UiUpdateKind::tool_start, .text = std::string{name}, .tool_id = std::string{id}});
}

void DesktopEventSink::on_done(core::StopReason stop_reason) {
  deliver(UiUpdate{.kind = UiUpdateKind::done, .stop_reason = stop_reason});
}

void DesktopEventSink::deliver(UiUpdate update) {
  ++delivered_;
  if (deliver_) {
    deliver_(update);
  }
}

ChatBridge::ChatBridge(ChatBridgeOptions options)
    : executor_{options.executor}, prompts_{options.executor, options.prompt_capacity},
      updates_{options.executor, options.update_capacity}, turn_cancel_{std::in_place},
      sink_{[this](const UiUpdate& update) {
        if (!updates_.try_send(update).has_value()) {
          ++dropped_;
        }
      }} {}

core::Result<void> ChatBridge::submit(std::string prompt) {
  return prompts_.try_send(std::move(prompt));
}

void ChatBridge::request_stop() {
  // Emit on the runtime executor so a UI-thread caller does not touch the
  // signal from another thread; the runtime side owns begin_turn/emit ordering.
  asio::post(executor_, [this] {
    if (turn_cancel_.has_value()) {
      turn_cancel_->emit(asio::cancellation_type::terminal);
    }
  });
}

asio::cancellation_slot ChatBridge::cancellation_slot() noexcept {
  return turn_cancel_->slot();
}

asio::cancellation_slot ChatBridge::begin_turn() noexcept {
  turn_cancel_.emplace();
  return turn_cancel_->slot();
}

async::Awaitable<core::Result<std::string>> ChatBridge::next_prompt() {
  co_return co_await prompts_.receive();
}

std::size_t ChatBridge::drain(ChatViewModel& view_model) {
  std::size_t applied = 0;
  while (true) {
    auto received = updates_.try_receive();
    if (!received.has_value()) {
      break;  // queue closed
    }
    if (!received->has_value()) {
      break;  // queue empty
    }
    view_model.apply(**received);
    ++applied;
  }
  return applied;
}

void ChatBridge::close() noexcept {
  // Close the input direction only. A closed prompt channel drains its queued
  // prompts, then fails `next_prompt()` with `cancelled`, which is how the
  // session loop winds down. The update channel stays open so a final in-flight
  // turn's buffered deltas remain drainable into the view-model; it is torn down
  // when the bridge is destroyed. Closing both here would drop those deltas
  // (a closed channel rejects `try_send`), defeating drain-to-completion.
  prompts_.close();
}

async::Awaitable<core::Result<void>> run_chat_session(ChatBridge& bridge, TurnRunner run_turn) {
  const auto executor = co_await asio::this_coro::executor;
  while (true) {
    auto prompt = co_await bridge.next_prompt();
    if (!prompt) {
      // Closed (or the idle wait was cancelled): the session is done.
      co_return core::Result<void>{};
    }

    // Fresh per-turn cancellation slot so a stop ends this turn only; a turn
    // error (including cancellation) is swallowed here and the loop continues.
    const auto slot = bridge.begin_turn();
    auto outcome = co_await asio::co_spawn(executor,
                                           run_turn(std::move(*prompt), bridge.event_sink()),
                                           asio::bind_cancellation_slot(slot, asio::use_awaitable));
    static_cast<void>(outcome);
  }
}

}  // namespace orangutan::desktop
