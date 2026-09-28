#include "protocol.hpp"

namespace orangutan::channel {
namespace {
using namespace detail;
class QQ final : public Adapter {
public:
  Platform platform() const noexcept override {
    return Platform::qq;
  }
  Capabilities capabilities(ChatKind kind) const noexcept override {
    return {kind == ChatKind::direct ? Activity::expiring : Activity::none, false, 2000};
  }
  Result<std::optional<Message>> decode(std::string_view envelope, const Account& account) const override {
    auto parsed = parse(envelope);
    if (!parsed)
      return std::unexpected(parsed.error());
    try {
      if (parsed->value("op", -1) != 0)
        return std::optional<Message>{};
      auto type = parsed->at("t").get<std::string>();
      if (type != "C2C_MESSAGE_CREATE" && type != "GROUP_AT_MESSAGE_CREATE")
        return std::optional<Message>{};
      const auto& msg = parsed->at("d");
      const auto& author = msg.at("author");
      bool group = type == "GROUP_AT_MESSAGE_CREATE";
      auto sender = id(author.at(group ? "member_openid" : "user_openid"));
      auto text = msg.at("content").get<std::string>();
      if (!account.bot_id.empty()) {
        const auto mention = "<@!" + account.bot_id + ">";
        if (text.starts_with(mention))
          text.erase(0, mention.size());
      }
      if (auto pos = text.find_first_not_of(" \t\r\n"); pos != std::string::npos)
        text.erase(0, pos);
      else
        return std::optional<Message>{};
      auto message_id = id(msg.at("id"));
      Message m{{platform(),
                 account.id,
                 group ? ChatKind::group : ChatKind::direct,
                 group ? id(msg.at("group_openid")) : sender,
                 {}},
                message_id,
                message_id,
                sender,
                std::move(text)};
      return checked(std::move(m));
    } catch (const Json::exception&) {
      return std::unexpected(Error::parsing("invalid QQ message"));
    }
  }
  Result<Request> reply(const Message& m, std::string_view text, std::size_t part) const override {
    auto valid = validate(m, platform());
    if (!valid)
      return std::unexpected(valid.error());
    if (!m.conversation.thread.empty() || part >= 5)
      return std::unexpected(Error::invalid_argument("QQ passive replies support at most five parts and no threads"));
    return Request{"POST",
                   std::string{m.conversation.kind == ChatKind::group ? "/v2/groups/" : "/v2/users/"} +
                       segment(m.conversation.chat) + "/messages",
                   Json{{"content", text},
                        {"msg_type", 0},
                        {"msg_id", m.message_id},
                        {"msg_seq", part + (m.conversation.kind == ChatKind::direct ? 3 : 1)}}
                       .dump()};
  }
  Result<std::optional<Request>> activity(const Message& m, std::optional<Receipt> receipt) const override {
    auto valid = validate(m, platform());
    if (!valid)
      return std::unexpected(valid.error());
    if (m.conversation.kind != ChatKind::direct)
      return std::optional<Request>{};
    return std::optional{Request{"POST",
                                 "/v2/users/" + segment(m.conversation.chat) + "/messages",
                                 Json{{"msg_type", 6},
                                      {"msg_id", m.message_id},
                                      {"msg_seq", receipt ? 2 : 1},
                                      {"input_notify", {{"input_type", receipt ? 2 : 1}, {"input_second", 60}}}}
                                     .dump()}};
  }
  Result<Receipt> accept(const Response& response) const override {
    return detail::accept(platform(), response);
  }
};
}  // namespace
const Adapter& qq() noexcept {
  static const QQ adapter;
  return adapter;
}
}  // namespace orangutan::channel
