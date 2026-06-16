// src/oran-desktop/chat_bridge.cpp — always-built desktop bridge implementation.

#include <oran/desktop/chat_bridge.hpp>

#include <utility>

#include <asio/cancellation_type.hpp>

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
    : prompts_{options.executor, options.prompt_capacity}, updates_{options.executor, options.update_capacity},
      sink_{[this](const UiUpdate& update) {
        if (!updates_.try_send(update).has_value()) {
          ++dropped_;
        }
      }} {}

core::Result<void> ChatBridge::submit(std::string prompt) {
  return prompts_.try_send(std::move(prompt));
}

void ChatBridge::request_stop() noexcept {
  cancel_.emit(asio::cancellation_type::terminal);
}

asio::cancellation_slot ChatBridge::cancellation_slot() noexcept {
  return cancel_.slot();
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
  prompts_.close();
  updates_.close();
}

}  // namespace orangutan::desktop
