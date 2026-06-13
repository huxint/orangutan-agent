// src/oran-channel/dispatch.cpp — channel prompt dispatch implementation.

#include <oran/channel/dispatch.hpp>

#include <expected>
#include <ranges>
#include <string>
#include <utility>

#include <oran/async/channel.hpp>
#include <oran/core/content.hpp>
#include <oran/core/error.hpp>

#include <oran/channel/manager.hpp>

namespace orangutan::channel {

core::Result<ChannelPromptRunRequest> make_prompt_run_request(const InboundMessage& message) {
  auto texts = message.content |
               std::views::transform([](const core::Content& block) { return core::text_view(block); }) |
               std::views::filter([](const auto& text) { return text.has_value() && !text->empty(); }) |
               std::views::transform([](const auto& text) { return *text; });
  auto prompt = texts | std::views::join_with('\n') | std::ranges::to<std::string>();
  if (prompt.empty()) {
    return std::unexpected(core::Error::invalid_argument("inbound channel message has no text content")
                               .with("channel_id", message.channel_id)
                               .with("conversation_id", message.conversation_id));
  }

  return ChannelPromptRunRequest{
      .channel_id = message.channel_id,
      .conversation_id = message.conversation_id,
      .user_id = message.user_id,
      .display_name = message.display_name,
      .prompt = std::move(prompt),
      .origin = message.origin,
      .caps = message.caps,
      .received_at = message.received_at,
  };
}

OutboundMessage make_reply_message(const InboundMessage& message, std::string text) {
  auto reply = OutboundMessage{
      .conversation_id = message.conversation_id,
      .content = {core::TextContent{.text = std::move(text)}},
      .reactions = {},
  };
  if (!message.replies_to.empty()) {
    reply.reply_to_message_id = message.replies_to.front().message_id;
    reply.thread_id = message.replies_to.front().thread_id;
  }
  return reply;
}

async::Awaitable<core::Result<DeliveryReceipt>> dispatch_one(ChannelManager& manager,
                                                             const ChannelPromptRunner& runner) {
  if (!runner) {
    co_return std::unexpected(core::Error::invalid_argument("channel prompt runner is null"));
  }

  auto message = co_await manager.inbound().receive();
  if (!message) {
    co_return std::unexpected(std::move(message).error());
  }

  auto request = make_prompt_run_request(*message);
  if (!request) {
    co_return std::unexpected(std::move(request).error());
  }

  auto result = co_await runner(std::move(*request));
  if (!result) {
    co_return std::unexpected(std::move(result).error());
  }

  co_return co_await manager.send(message->channel_id, make_reply_message(*message, std::move(result->text)));
}

}  // namespace orangutan::channel
