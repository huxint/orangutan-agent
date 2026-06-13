// src/oran-channel-qq/channel.cpp — QQ channel trait adapter.

#include <oran/channel-qq/channel.hpp>

#include <cctype>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <oran/core/content.hpp>
#include <oran/core/error.hpp>

#include <oran/channel-qq/api_client.hpp>
#include <oran/channel-qq/gateway_transport.hpp>

namespace orangutan::channel::qq {

namespace {

using json = nlohmann::json;

[[nodiscard]] QqChannelOptions normalize(QqChannelOptions options) {
  if (options.id.empty()) {
    options.id = "qq-main";
  }
  if (options.capabilities.max_text_bytes == 0) {
    options.capabilities.max_text_bytes = 5'000;
  }
  return options;
}

[[nodiscard]] core::Error not_started(std::string_view channel_id) {
  return core::Error{core::ErrorKind::conflict, "qq channel not started"}.with("channel_id", std::string{channel_id});
}

[[nodiscard]] core::Error stopped(std::string_view channel_id) {
  return core::Error{core::ErrorKind::conflict, "qq channel cannot restart after stop"}.with("channel_id",
                                                                                             std::string{channel_id});
}

[[nodiscard]] core::Error parse_error(std::string message, std::string_view event_type) {
  return core::Error::parsing(std::move(message)).with("event_type", std::string{event_type});
}

[[nodiscard]] std::optional<std::string> scalar_string(const json& object, std::string_view key) {
  if (!object.is_object()) {
    return std::nullopt;
  }
  const auto it = object.find(std::string{key});
  if (it == object.end() || it->is_null()) {
    return std::nullopt;
  }
  if (it->is_string()) {
    return it->get<std::string>();
  }
  if (it->is_number_unsigned()) {
    return std::to_string(it->get<unsigned long long>());
  }
  if (it->is_number_integer()) {
    return std::to_string(it->get<long long>());
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::string> first_scalar_string(const json& object,
                                                             std::initializer_list<std::string_view> keys) {
  for (const auto key : keys) {
    auto value = scalar_string(object, key);
    if (value.has_value() && !value->empty()) {
      return value;
    }
  }
  return std::nullopt;
}

[[nodiscard]] const json& object_field_or_empty(const json& object, std::string_view key) {
  static const json kEmpty = json::object();
  if (!object.is_object()) {
    return kEmpty;
  }
  const auto it = object.find(std::string{key});
  return it != object.end() && it->is_object() ? *it : kEmpty;
}

[[nodiscard]] std::string trim_ascii(std::string text) {
  auto view = std::string_view{text};
  while (!view.empty() && std::isspace(static_cast<unsigned char>(view.front())) != 0) {
    view.remove_prefix(1);
  }
  while (!view.empty() && std::isspace(static_cast<unsigned char>(view.back())) != 0) {
    view.remove_suffix(1);
  }
  return std::string{view};
}

[[nodiscard]] std::string strip_mentions(std::string_view content) {
  std::string stripped;
  stripped.reserve(content.size());

  for (std::size_t cursor = 0; cursor < content.size();) {
    if (content[cursor] == '<' && cursor + 2 < content.size() && content[cursor + 1] == '@') {
      if (const auto close = content.find('>', cursor + 2); close != std::string_view::npos) {
        cursor = close + 1;
        continue;
      }
    }
    stripped.push_back(content[cursor]);
    ++cursor;
  }

  return trim_ascii(std::move(stripped));
}

[[nodiscard]] core::Result<std::string> content_text(const json& data, std::string_view event_type) {
  auto content = scalar_string(data, "content");
  if (!content.has_value()) {
    if (!data.is_object() || !data.contains("content") || data.at("content").is_null()) {
      return std::string{};
    }
    return std::unexpected(
        parse_error("qq message content is not a scalar string", event_type).with("field", "content"));
  }
  return *std::move(content);
}

[[nodiscard]] std::vector<core::Content> text_blocks(std::string text) {
  return {core::TextContent{.text = std::move(text)}};
}

[[nodiscard]] std::vector<orangutan::channel::Reference> references_from(const json& data) {
  auto message_id = first_scalar_string(data, {"id", "msg_id", "message_id"});
  if (!message_id.has_value()) {
    return {};
  }
  return {orangutan::channel::Reference{.message_id = std::move(*message_id)}};
}

[[nodiscard]] core::Error
send_argument_error(std::string message, std::string_view channel_id, std::string_view conversation_id) {
  return core::Error::invalid_argument(std::move(message))
      .with("channel_id", std::string{channel_id})
      .with("conversation_id", std::string{conversation_id});
}

[[nodiscard]] core::Result<std::string> outbound_text(const orangutan::channel::OutboundMessage& message,
                                                      std::string_view channel_id,
                                                      std::size_t max_text_bytes) {
  auto texts = message.content |
               std::views::transform([](const core::Content& block) { return core::text_view(block); }) |
               std::views::filter([](const auto& text) { return text.has_value() && !text->empty(); }) |
               std::views::transform([](const auto& text) { return *text; });
  auto text = texts | std::views::join_with('\n') | std::ranges::to<std::string>();
  if (text.empty()) {
    return std::unexpected(send_argument_error("qq outbound text reply requires non-empty text content",
                                               channel_id,
                                               message.conversation_id));
  }
  if (max_text_bytes > 0 && text.size() > max_text_bytes) {
    return std::unexpected(
        send_argument_error("qq outbound text reply exceeds max text bytes", channel_id, message.conversation_id)
            .with("text_bytes", std::to_string(text.size()))
            .with("max_text_bytes", std::to_string(max_text_bytes)));
  }
  return text;
}

[[nodiscard]] core::Result<std::string> send_path_for(std::string_view conversation_id, std::string_view channel_id) {
  static constexpr std::string_view kC2cPrefix = "c2c:";
  static constexpr std::string_view kGroupPrefix = "group:";

  auto segment_or_error = [&](std::string_view segment, std::string_view kind) -> core::Result<std::string> {
    if (segment.empty() || segment.contains('/')) {
      return std::unexpected(
          send_argument_error("qq outbound conversation id has an invalid target id", channel_id, conversation_id)
              .with("conversation_kind", std::string{kind}));
    }
    return std::string{segment};
  };

  if (conversation_id.starts_with(kC2cPrefix)) {
    auto openid = segment_or_error(conversation_id.substr(kC2cPrefix.size()), "c2c");
    if (!openid) {
      return std::unexpected(std::move(openid).error());
    }
    return "/v2/users/" + *std::move(openid) + "/messages";
  }
  if (conversation_id.starts_with(kGroupPrefix)) {
    auto group_openid = segment_or_error(conversation_id.substr(kGroupPrefix.size()), "group");
    if (!group_openid) {
      return std::unexpected(std::move(group_openid).error());
    }
    return "/v2/groups/" + *std::move(group_openid) + "/messages";
  }

  return std::unexpected(
      send_argument_error("qq outbound conversation id must start with c2c: or group:", channel_id, conversation_id));
}

[[nodiscard]] core::Result<std::string> receipt_message_id(const ApiResponse& response, std::string_view path) {
  const auto payload = json::parse(response.body, nullptr, /*allow_exceptions=*/false);
  if (!payload.is_object()) {
    return std::unexpected(core::Error::parsing("qq send response body is not a json object")
                               .with("path", std::string{path})
                               .with("http_status", std::to_string(response.http_status)));
  }

  auto message_id = first_scalar_string(payload, {"id", "msg_id", "message_id"});
  if (!message_id.has_value()) {
    return std::unexpected(core::Error::parsing("qq send response is missing message id")
                               .with("path", std::string{path})
                               .with("http_status", std::to_string(response.http_status)));
  }
  return *std::move(message_id);
}

[[nodiscard]] orangutan::channel::InboundMessage make_message(QqDispatchNormalizationOptions options,
                                                              std::string conversation_id,
                                                              std::string user_id,
                                                              std::string display_name,
                                                              std::string text,
                                                              const json& data) {
  return orangutan::channel::InboundMessage{
      .channel_id = std::move(options.channel_id),
      .conversation_id = std::move(conversation_id),
      .user_id = std::move(user_id),
      .display_name = std::move(display_name),
      .content = text_blocks(std::move(text)),
      .replies_to = references_from(data),
      .received_at = options.received_at,
      .origin = {.kind = "channel", .source = "qq"},
      .caps = options.capabilities,
  };
}

[[nodiscard]] core::Result<std::optional<orangutan::channel::InboundMessage>>
normalize_c2c(const GatewayDispatch& dispatch, const json& data, QqDispatchNormalizationOptions options) {
  const auto& author = object_field_or_empty(data, "author");
  auto openid = first_scalar_string(author, {"user_openid", "id"});
  if (!openid.has_value()) {
    openid = first_scalar_string(data, {"user_openid", "openid"});
  }
  if (!openid.has_value()) {
    return std::unexpected(
        parse_error("qq c2c message is missing user openid", dispatch.event_type).with("field", "author.user_openid"));
  }

  auto text = content_text(data, dispatch.event_type);
  if (!text) {
    return std::unexpected(std::move(text).error());
  }

  auto display_name = first_scalar_string(author, {"username", "nick", "member_openid", "id"}).value_or(*openid);
  auto message =
      make_message(std::move(options), "c2c:" + *openid, *openid, std::move(display_name), std::move(*text), data);
  return std::optional<orangutan::channel::InboundMessage>{std::move(message)};
}

[[nodiscard]] core::Result<std::optional<orangutan::channel::InboundMessage>>
normalize_group(const GatewayDispatch& dispatch, const json& data, QqDispatchNormalizationOptions options) {
  auto group_openid = scalar_string(data, "group_openid");
  if (!group_openid.has_value() || group_openid->empty()) {
    return std::unexpected(
        parse_error("qq group message is missing group openid", dispatch.event_type).with("field", "group_openid"));
  }

  const auto& author = object_field_or_empty(data, "author");
  auto user_id = first_scalar_string(author, {"member_openid", "user_openid", "id"});
  if (!user_id.has_value()) {
    user_id = first_scalar_string(data, {"member_openid", "user_openid", "openid"});
  }
  if (!user_id.has_value()) {
    return std::unexpected(parse_error("qq group message is missing sender openid", dispatch.event_type)
                               .with("field", "author.member_openid"));
  }

  auto text = content_text(data, dispatch.event_type);
  if (!text) {
    return std::unexpected(std::move(text).error());
  }

  auto display_name = first_scalar_string(author, {"username", "nick", "member_openid", "id"}).value_or(*user_id);
  auto message = make_message(std::move(options),
                              "group:" + *group_openid,
                              *user_id,
                              std::move(display_name),
                              strip_mentions(*text),
                              data);
  return std::optional<orangutan::channel::InboundMessage>{std::move(message)};
}

}  // namespace

core::Result<std::optional<orangutan::channel::InboundMessage>>
normalize_gateway_dispatch(GatewayDispatch dispatch, QqDispatchNormalizationOptions options) {
  json data;
  try {
    data = json::parse(dispatch.data_json);
  } catch (const json::exception& e) {
    return std::unexpected(
        parse_error("qq gateway dispatch data is not valid json", dispatch.event_type).with("what", e.what()));
  }

  if (!data.is_object()) {
    return std::unexpected(parse_error("qq gateway dispatch data is not an object", dispatch.event_type));
  }

  if (dispatch.event_type == "C2C_MESSAGE_CREATE") {
    return normalize_c2c(dispatch, data, std::move(options));
  }
  if (dispatch.event_type == "GROUP_AT_MESSAGE_CREATE" || dispatch.event_type == "GROUP_MESSAGE_CREATE") {
    return normalize_group(dispatch, data, std::move(options));
  }

  return std::optional<orangutan::channel::InboundMessage>{};
}

struct QqChannel::Impl {
  Impl(GatewayTransport gateway_transport, ApiClient& client, QqChannelOptions opts)
      : transport{std::move(gateway_transport)}, api_client{&client}, options{normalize(std::move(opts))} {}

  [[nodiscard]] std::uint32_t next_msg_seq() noexcept {
    const auto current = send_sequence;
    send_sequence = send_sequence == 65'535 ? 1 : send_sequence + 1;
    return current;
  }

  GatewayTransport transport;
  ApiClient* api_client;
  QqChannelOptions options;
  std::uint32_t send_sequence{1};
  bool started{false};
  bool permanently_stopped{false};
};

QqChannel::QqChannel(GatewayTransport transport, ApiClient& api_client, QqChannelOptions options)
    : impl_{std::make_unique<Impl>(std::move(transport), api_client, std::move(options))} {}

QqChannel::~QqChannel() = default;

QqChannel::QqChannel(QqChannel&&) noexcept = default;

QqChannel& QqChannel::operator=(QqChannel&&) noexcept = default;

std::string_view QqChannel::id() const noexcept {
  return impl_->options.id;
}

std::string_view QqChannel::kind() const noexcept {
  return "qq";
}

orangutan::channel::Capabilities QqChannel::capabilities() const noexcept {
  return impl_->options.capabilities;
}

async::Awaitable<core::Result<void>> QqChannel::start() {
  if (impl_->permanently_stopped) {
    co_return std::unexpected(stopped(impl_->options.id));
  }
  impl_->started = true;
  co_return core::Result<void>{};
}

async::Awaitable<core::Result<void>> QqChannel::stop() {
  if (!impl_->started) {
    co_return core::Result<void>{};
  }
  impl_->started = false;
  impl_->permanently_stopped = true;
  co_return co_await impl_->transport.close();
}

async::Awaitable<core::Result<orangutan::channel::InboundMessage>> QqChannel::next_message() {
  if (!impl_->started) {
    co_return std::unexpected(not_started(impl_->options.id));
  }

  for (;;) {
    auto dispatch = co_await impl_->transport.next_dispatch();
    if (!dispatch) {
      co_return std::unexpected(std::move(dispatch).error());
    }

    auto message = normalize_gateway_dispatch(std::move(*dispatch),
                                              QqDispatchNormalizationOptions{
                                                  .channel_id = impl_->options.id,
                                                  .capabilities = impl_->options.capabilities,
                                                  .received_at = core::time::now_utc(),
                                              });
    if (!message) {
      co_return std::unexpected(std::move(message).error());
    }
    if (message->has_value()) {
      co_return std::move(**message);
    }
  }
}

async::Awaitable<core::Result<orangutan::channel::DeliveryReceipt>>
QqChannel::send(orangutan::channel::OutboundMessage message) {
  if (!impl_->started) {
    co_return std::unexpected(not_started(impl_->options.id));
  }
  if (!message.reply_to_message_id.has_value() || message.reply_to_message_id->empty()) {
    co_return std::unexpected(send_argument_error("qq passive text reply requires reply_to_message_id",
                                                  impl_->options.id,
                                                  message.conversation_id)
                                  .with("reason", "qq_passive_reply_requires_message_id"));
  }

  auto path = send_path_for(message.conversation_id, impl_->options.id);
  if (!path) {
    co_return std::unexpected(std::move(path).error());
  }

  auto text = outbound_text(message, impl_->options.id, impl_->options.capabilities.max_text_bytes);
  if (!text) {
    co_return std::unexpected(std::move(text).error());
  }

  const auto body = json{
      {"content", *std::move(text)},
      {"msg_type", 0},
      {"msg_id", *message.reply_to_message_id},
      {"msg_seq", impl_->next_msg_seq()},
  };
  auto response = co_await impl_->api_client->post(*path, body.dump());
  if (!response) {
    co_return std::unexpected(std::move(response).error().with("channel_id", impl_->options.id));
  }

  auto message_id = receipt_message_id(*response, *path);
  if (!message_id) {
    co_return std::unexpected(std::move(message_id).error().with("channel_id", impl_->options.id));
  }

  co_return orangutan::channel::DeliveryReceipt{
      .message_id = std::move(*message_id),
      .accepted_at = core::time::now_utc(),
  };
}

bool QqChannel::started() const noexcept {
  return impl_->started;
}

}  // namespace orangutan::channel::qq
