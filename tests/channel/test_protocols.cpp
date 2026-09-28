#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <oran/channel/adapter.hpp>
#include <oran/core/error.hpp>

namespace channel = orangutan::channel;
using Json = nlohmann::json;

TEST_CASE("Telegram preserves account chat topic and reply identity") {
  auto result = channel::telegram().decode(
      R"({"update_id":19,"message":{"message_id":23,"message_thread_id":42,"chat":{"id":-100123,"type":"supergroup"},"from":{"id":9,"is_bot":false},"text":"hello"}})",
      {"bot-one", "7"});
  REQUIRE(result);
  REQUIRE(*result);
  const auto& m = **result;
  CHECK(m.conversation.chat == "-100123");
  CHECK(m.conversation.thread == "42");
  CHECK(m.conversation.account == "bot-one");
  auto reply = channel::telegram().reply(m, "<plain> *text*", 0);
  REQUIRE(reply);
  auto body = Json::parse(reply->body);
  CHECK(body["chat_id"] == -100123);
  CHECK(body["message_thread_id"] == 42);
  CHECK(body["reply_parameters"]["message_id"] == 23);
  CHECK_FALSE(body.contains("parse_mode"));
  auto typing = channel::telegram().activity(m, {});
  REQUIRE(typing);
  REQUIRE(*typing);
  CHECK((**typing).path == "/sendChatAction");
  CHECK(Json::parse((**typing).body)["message_thread_id"] == 42);
  auto stop = channel::telegram().activity(m, channel::Receipt{});
  REQUIRE(stop);
  CHECK_FALSE(*stop);
  auto other = m.conversation;
  other.account = "bot-two";
  CHECK(channel::conversation_key(other) != channel::conversation_key(m.conversation));
  other = m.conversation;
  other.thread = "43";
  CHECK(channel::conversation_key(other) != channel::conversation_key(m.conversation));
}

TEST_CASE("QQ official group and C2C passive replies carry message sequences") {
  for (bool group : {false, true}) {
    Json event{{"op", 0},
               {"t", group ? "GROUP_AT_MESSAGE_CREATE" : "C2C_MESSAGE_CREATE"},
               {"d",
                {{"id", "incoming"},
                 {"content", " hello"},
                 {"group_openid", "group"},
                 {"author", {{group ? "member_openid" : "user_openid", "user"}}}}}};
    auto result = channel::qq().decode(event.dump(), {"account", "bot"});
    REQUIRE(result);
    REQUIRE(*result);
    auto reply = channel::qq().reply(**result, "answer", 1);
    REQUIRE(reply);
    CHECK(reply->path == (group ? "/v2/groups/group/messages" : "/v2/users/user/messages"));
    auto body = Json::parse(reply->body);
    CHECK(body["msg_id"] == "incoming");
    CHECK(body["msg_seq"] == (group ? 2 : 4));
    CHECK(body["msg_type"] == 0);
    CHECK_FALSE(channel::qq().reply(**result, "answer", 5));
    auto typing = channel::qq().activity(**result, {});
    REQUIRE(typing);
    if (group)
      CHECK_FALSE(*typing);
    else {
      REQUIRE(*typing);
      auto status = Json::parse((**typing).body);
      CHECK(status["msg_type"] == 6);
      CHECK(status["msg_seq"] == 1);
      CHECK(status["input_notify"]["input_type"] == 1);
      auto stop = channel::qq().activity(**result, channel::Receipt{"status"});
      REQUIRE(stop);
      REQUIRE(*stop);
      CHECK(Json::parse((**stop).body)["input_notify"]["input_type"] == 2);
    }
  }
}

TEST_CASE("Feishu decodes user text and removes only its own typing reaction") {
  auto result = channel::feishu().decode(
      R"({"schema":"2.0","header":{"event_id":"evt","event_type":"im.message.receive_v1"},"event":{"sender":{"sender_type":"user","sender_id":{"open_id":"user"}},"message":{"message_id":"om_1","chat_id":"oc_1","chat_type":"group","thread_id":"omt_1","message_type":"text","content":"{\"text\":\"@_user_1 hello\"}","mentions":[{"key":"@_user_1","id":{"open_id":"bot"}}]}}})",
      {"account", "bot"});
  REQUIRE(result);
  REQUIRE(*result);
  CHECK((**result).text == "hello");
  auto reply = channel::feishu().reply(**result, "中文 \"quoted\"", 0);
  REQUIRE(reply);
  CHECK(reply->path == "/open-apis/im/v1/messages/om_1/reply");
  auto body = Json::parse(reply->body);
  CHECK(body["reply_in_thread"] == true);
  CHECK(Json::parse(body["content"].get<std::string>())["text"] == "中文 \"quoted\"");
  auto start = channel::feishu().activity(**result, {});
  REQUIRE(start);
  REQUIRE(*start);
  CHECK(Json::parse((**start).body)["reaction_type"]["emoji_type"] == "Typing");
  auto receipt = channel::feishu().accept({200, R"({"code":0,"data":{"reaction_id":"reaction/1"}})", {}});
  REQUIRE(receipt);
  auto stop = channel::feishu().activity(**result, *receipt);
  REQUIRE(stop);
  REQUIRE(*stop);
  CHECK((**stop).method == "DELETE");
  CHECK((**stop).path.ends_with("/reactions/reaction%2F1"));
}

TEST_CASE("Channel decoders reject malformed supported events and ignore unrelated events") {
  for (auto adapter : {&channel::telegram(), &channel::qq(), &channel::feishu()}) {
    CHECK_FALSE(adapter->decode("not json", {"account", {}}));
    auto ignored = adapter->decode("{}", {"account", {}});
    REQUIRE(ignored);
    CHECK_FALSE(*ignored);
  }
  CHECK_FALSE(channel::telegram().decode(R"({"message":{"text":4}})", {"account", {}}));
  CHECK_FALSE(channel::qq().decode(R"({"op":0,"t":"C2C_MESSAGE_CREATE","d":null})", {"account", {}}));
  CHECK_FALSE(
      channel::feishu().decode(R"({"header":{"event_type":"im.message.receive_v1"},"event":null})", {"account", {}}));
  auto bot = channel::telegram().decode(R"({"message":{"text":"loop","from":{"is_bot":true}}})", {"account", {}});
  REQUIRE(bot);
  CHECK_FALSE(*bot);
}

TEST_CASE("Channel API failures do not echo upstream bodies and preserve retry delays") {
  auto limited = channel::telegram().accept(
      {200, R"({"ok":false,"error_code":429,"description":"SECRET","parameters":{"retry_after":7}})", {}});
  REQUIRE_FALSE(limited);
  CHECK(limited.error().kind() == orangutan::core::ErrorKind::rate_limit);
  CHECK(limited.error().retry_after() == std::chrono::seconds{7});
  CHECK_FALSE(limited.error().message().contains("SECRET"));
  CHECK_FALSE(channel::feishu().accept({200, R"({"code":999,"msg":"SECRET"})", {}}));
  CHECK_FALSE(channel::qq().accept({200, R"({"code":123,"message":"SECRET"})", {}}));
  CHECK_FALSE(channel::telegram().accept({200, R"({"ok":"true"})", {}}));
  CHECK_FALSE(channel::qq().accept({200, "{}", {}}));
}

TEST_CASE("Channel text splitting preserves Unicode and rejects malformed UTF-8") {
  const std::string text = "你好🙂world";
  auto parts = channel::split_text(text, 4);
  REQUIRE(parts);
  std::string joined;
  for (const auto& part : *parts) {
    CHECK(part.size() <= 4);
    joined += part;
  }
  CHECK(joined == text);
  CHECK_FALSE(channel::split_text(std::string{"\xc0\x80", 2}, 4));
  CHECK_FALSE(channel::split_text(std::string{"\xed\xa0\x80", 3}, 4));
  CHECK_FALSE(channel::split_text(std::string{"\xf4\x90\x80\x80", 4}, 4));
  CHECK_FALSE(channel::split_text("x", 0));
}
