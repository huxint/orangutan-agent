// tests/channel-qq/test_channel.cpp — QQ Channel trait adapter coverage.

#include <oran/channel-qq/api_client.hpp>
#include <oran/channel-qq/channel.hpp>
#include <oran/channel-qq/gateway_transport.hpp>
#include <oran/channel-qq/token_store.hpp>

#include <chrono>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/thread_pool.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <oran/async.hpp>
#include <oran/channel.hpp>
#include <oran/core/content.hpp>
#include <oran/core/error.hpp>
#include <oran/http/client.hpp>

#include "../test-helpers/run_async.hpp"
#include "../test-helpers/ws_test_server.hpp"
#include "scripted_http_server.hpp"

using namespace std::chrono_literals;

namespace {

namespace async = orangutan::async;
namespace channel = orangutan::channel;
namespace core = orangutan::core;
namespace http = orangutan::http;
namespace qq = orangutan::channel::qq;
namespace test = orangutan::tests;
namespace ws = orangutan::tests::ws;

using json = nlohmann::json;

[[nodiscard]] test::ScriptedResponse token_ok(std::string token) {
  return test::ScriptedResponse{
      .status = 200,
      .body = R"({"access_token":")" + std::move(token) + R"(","expires_in":7200})",
      .headers = {{"Content-Type", "application/json"}},
      .delay = 0ms,
  };
}

[[nodiscard]] test::ScriptedResponse api_response(int status, std::string body) {
  return test::ScriptedResponse{
      .status = status,
      .body = std::move(body),
      .headers = {{"Content-Type", "application/json"}},
      .delay = 0ms,
  };
}

[[nodiscard]] qq::TokenStoreOptions token_options(const test::ScriptedHttpServer& server) {
  auto options = qq::TokenStoreOptions{};
  options.token_url = server.url("/app/getAppAccessToken");
  options.request_timeout = 2s;
  return options;
}

[[nodiscard]] qq::ApiClientOptions api_options(const test::ScriptedHttpServer& server) {
  auto options = qq::ApiClientOptions{};
  options.base_url = server.base_url();
  options.request_timeout = 2s;
  return options;
}

[[nodiscard]] qq::GatewayTransportOptions transport_options(const ws::ScriptedWsServer& server) {
  auto options = qq::GatewayTransportOptions{};
  options.gateway_url = server.url();
  options.handshake_timeout = 2s;
  options.frame_timeout = 300ms;
  options.reconnect_delays = {1ms, 1ms, 1ms, 1ms, 1ms, 1ms};
  options.max_reconnect_attempts = 5;
  return options;
}

[[nodiscard]] std::string hello_frame(std::chrono::milliseconds interval) {
  return json{{"op", 10}, {"d", {{"heartbeat_interval", interval.count()}}}}.dump();
}

[[nodiscard]] std::string ready_frame(std::uint32_t seq, std::string session_id) {
  return json{{"op", 0}, {"s", seq}, {"t", "READY"}, {"d", {{"session_id", std::move(session_id)}}}}.dump();
}

[[nodiscard]] std::string dispatch_frame(std::uint32_t seq, std::string event_type, json data) {
  return json{{"op", 0}, {"s", seq}, {"t", std::move(event_type)}, {"d", std::move(data)}}.dump();
}

[[nodiscard]] qq::GatewayDispatch dispatch(std::string event_type, json data) {
  return qq::GatewayDispatch{.event_type = std::move(event_type), .data_json = std::move(data).dump()};
}

[[nodiscard]] std::string_view request_body(std::string_view request) {
  const auto header_end = request.find("\r\n\r\n");
  if (header_end == std::string_view::npos) {
    return {};
  }
  return request.substr(header_end + 4);
}

struct ChannelFixture {
  ChannelFixture(const test::ScriptedHttpServer& token_server, const ws::ScriptedWsServer& gateway_server)
      : client{blocking.get_executor()}, tokens{client,
                                                qq::Credentials{.app_id = "test-app", .client_secret = "test-secret"},
                                                token_options(token_server)},
        api{client, tokens}, transport{tokens, transport_options(gateway_server)} {}

  asio::thread_pool blocking{1};
  http::Client client;
  qq::TokenStore tokens;
  qq::ApiClient api;
  qq::GatewayTransport transport;
};

struct SendFixture {
  explicit SendFixture(const test::ScriptedHttpServer& server)
      : client{blocking.get_executor()},
        tokens{client, qq::Credentials{.app_id = "test-app", .client_secret = "test-secret"}, token_options(server)},
        api{client, tokens, api_options(server)},
        transport{tokens, qq::GatewayTransportOptions{.gateway_url = "ws://127.0.0.1:1/gateway"}} {}

  asio::thread_pool blocking{1};
  http::Client client;
  qq::TokenStore tokens;
  qq::ApiClient api;
  qq::GatewayTransport transport;
};

struct OfflineFixture {
  OfflineFixture()
      : client{blocking.get_executor()},
        tokens{client,
               qq::Credentials{.app_id = "test-app", .client_secret = "test-secret"},
               qq::TokenStoreOptions{.token_url = "http://127.0.0.1:1/app/getAppAccessToken"}},
        api{client, tokens}, transport{tokens, qq::GatewayTransportOptions{.gateway_url = "ws://127.0.0.1:1/gateway"}} {
  }

  asio::thread_pool blocking{1};
  http::Client client;
  qq::TokenStore tokens;
  qq::ApiClient api;
  qq::GatewayTransport transport;
};

}  // namespace

TEST_CASE("QQ gateway dispatch normalization maps C2C messages", "[unit][channel-qq][channel]") {
  auto normalized = qq::normalize_gateway_dispatch(
      dispatch("C2C_MESSAGE_CREATE",
               json{{"id", "msg-1"},
                    {"content", "hello"},
                    {"author", {{"user_openid", "user-openid"}, {"username", "Operator"}}}}),
      qq::QqDispatchNormalizationOptions{.channel_id = "qq-ingress", .received_at = core::Time::epoch()});

  REQUIRE(normalized.has_value());
  REQUIRE(normalized->has_value());
  const auto& message = **normalized;
  REQUIRE(message.channel_id == "qq-ingress");
  REQUIRE(message.conversation_id == "c2c:user-openid");
  REQUIRE(message.user_id == "user-openid");
  REQUIRE(message.display_name == "Operator");
  REQUIRE(message.origin == channel::Origin{.kind = "channel", .source = "qq"});
  REQUIRE(message.caps.mentions);
  REQUIRE(message.caps.reply_quoting);
  REQUIRE(message.caps.max_text_bytes == 5'000);
  REQUIRE(message.replies_to.size() == 1);
  REQUIRE(message.replies_to.front().message_id == "msg-1");
  REQUIRE(message.content.size() == 1);
  REQUIRE(core::text_view(message.content.front()).value_or("") == "hello");
  REQUIRE(message.received_at == core::Time::epoch());
}

TEST_CASE("QQ gateway dispatch normalization maps group messages", "[unit][channel-qq][channel]") {
  auto normalized = qq::normalize_gateway_dispatch(
      dispatch("GROUP_AT_MESSAGE_CREATE",
               json{{"id", "group-msg-1"},
                    {"group_openid", "group-openid"},
                    {"content", "<@!bot-openid> hello group"},
                    {"author", {{"member_openid", "member-openid"}}}}),
      qq::QqDispatchNormalizationOptions{.channel_id = "qq-group", .received_at = core::Time::epoch()});

  REQUIRE(normalized.has_value());
  REQUIRE(normalized->has_value());
  const auto& message = **normalized;
  REQUIRE(message.channel_id == "qq-group");
  REQUIRE(message.conversation_id == "group:group-openid");
  REQUIRE(message.user_id == "member-openid");
  REQUIRE(message.display_name == "member-openid");
  REQUIRE(message.replies_to.size() == 1);
  REQUIRE(message.replies_to.front().message_id == "group-msg-1");
  REQUIRE(message.content.size() == 1);
  REQUIRE(core::text_view(message.content.front()).value_or("") == "hello group");
}

TEST_CASE("QQ gateway dispatch normalization skips unsupported events", "[unit][channel-qq][channel]") {
  auto normalized = qq::normalize_gateway_dispatch(dispatch("INTERACTION_CREATE", json{{"id", "interaction-1"}}));

  REQUIRE(normalized.has_value());
  REQUIRE_FALSE(normalized->has_value());
}

TEST_CASE("QQ gateway dispatch normalization rejects malformed message payloads", "[unit][channel-qq][channel]") {
  auto bad_json =
      qq::normalize_gateway_dispatch(qq::GatewayDispatch{.event_type = "C2C_MESSAGE_CREATE", .data_json = "not-json"});
  REQUIRE_FALSE(bad_json.has_value());
  REQUIRE(bad_json.error().kind() == core::ErrorKind::parsing);

  auto missing_group = qq::normalize_gateway_dispatch(dispatch("GROUP_MESSAGE_CREATE", json{{"content", "hi"}}));
  REQUIRE_FALSE(missing_group.has_value());
  REQUIRE(missing_group.error().kind() == core::ErrorKind::parsing);
}

TEST_CASE("QqChannel reports identity, lifecycle, and passive-reply guard", "[unit][channel-qq][channel][async]") {
  OfflineFixture fixture;
  qq::QqChannel adapter{std::move(fixture.transport), fixture.api, qq::QqChannelOptions{.id = "qq-main"}};

  REQUIRE(adapter.id() == "qq-main");
  REQUIRE(adapter.kind() == "qq");
  REQUIRE(adapter.capabilities().mentions);
  REQUIRE(adapter.capabilities().reply_quoting);
  REQUIRE_FALSE(adapter.started());

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto early_receive = co_await adapter.next_message();
    REQUIRE_FALSE(early_receive.has_value());
    REQUIRE(early_receive.error().kind() == core::ErrorKind::conflict);

    auto started = co_await adapter.start();
    REQUIRE(started.has_value());
    REQUIRE(adapter.started());

    auto sent =
        co_await adapter.send(channel::OutboundMessage{.conversation_id = "c2c:user", .content = {}, .reactions = {}});
    REQUIRE_FALSE(sent.has_value());
    REQUIRE(sent.error().kind() == core::ErrorKind::invalid_argument);

    auto stopped = co_await adapter.stop();
    REQUIRE(stopped.has_value());
    REQUIRE_FALSE(adapter.started());

    auto restarted = co_await adapter.start();
    REQUIRE_FALSE(restarted.has_value());
    REQUIRE(restarted.error().kind() == core::ErrorKind::conflict);
  });

  fixture.blocking.join();
}

TEST_CASE("QqChannel send posts C2C passive text replies", "[unit][channel-qq][channel][async]") {
  test::ScriptedHttpServer server{{token_ok("tok-1"), api_response(200, R"({"id":"out-1"})")}};
  SendFixture fixture{server};
  qq::QqChannel adapter{std::move(fixture.transport), fixture.api, qq::QqChannelOptions{.id = "qq-main"}};

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto started = co_await adapter.start();
    REQUIRE(started.has_value());

    auto receipt = co_await adapter.send(channel::OutboundMessage{
        .conversation_id = "c2c:user-openid",
        .content = {core::TextContent{.text = "hello c2c"}},
        .reply_to_message_id = "inbound-msg-1",
        .reactions = {},
    });
    REQUIRE(receipt.has_value());
    REQUIRE(receipt->message_id == "out-1");
  });

  REQUIRE(server.served_count() == 2);
  const auto request = server.request_text(1);
  REQUIRE(request.starts_with("POST /v2/users/user-openid/messages HTTP/1.1"));
  REQUIRE(request.contains("Authorization: QQBot tok-1"));
  const auto body = json::parse(request_body(request));
  REQUIRE(body.at("content") == "hello c2c");
  REQUIRE(body.at("msg_type") == 0);
  REQUIRE(body.at("msg_id") == "inbound-msg-1");
  REQUIRE(body.at("msg_seq") == 1);
  fixture.blocking.join();
}

TEST_CASE("QqChannel send posts group passive text replies", "[unit][channel-qq][channel][async]") {
  test::ScriptedHttpServer server{{token_ok("tok-1"), api_response(200, R"({"message_id":"out-group"})")}};
  SendFixture fixture{server};
  qq::QqChannel adapter{std::move(fixture.transport), fixture.api, qq::QqChannelOptions{.id = "qq-group"}};

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto started = co_await adapter.start();
    REQUIRE(started.has_value());

    auto receipt = co_await adapter.send(channel::OutboundMessage{
        .conversation_id = "group:group-openid",
        .content = {core::TextContent{.text = "first"}, core::TextContent{.text = "second"}},
        .reply_to_message_id = "group-inbound-msg",
        .reactions = {},
    });
    REQUIRE(receipt.has_value());
    REQUIRE(receipt->message_id == "out-group");
  });

  REQUIRE(server.served_count() == 2);
  const auto request = server.request_text(1);
  REQUIRE(request.starts_with("POST /v2/groups/group-openid/messages HTTP/1.1"));
  const auto body = json::parse(request_body(request));
  REQUIRE(body.at("content") == "first\nsecond");
  REQUIRE(body.at("msg_type") == 0);
  REQUIRE(body.at("msg_id") == "group-inbound-msg");
  REQUIRE(body.at("msg_seq") == 1);
  fixture.blocking.join();
}

TEST_CASE("QqChannel send rejects unsupported outbound shapes before HTTP", "[unit][channel-qq][channel][async]") {
  test::ScriptedHttpServer server{std::vector<test::ScriptedResponse>{}};
  SendFixture fixture{server};
  qq::QqChannel adapter{std::move(fixture.transport), fixture.api, qq::QqChannelOptions{.id = "qq-main"}};

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto started = co_await adapter.start();
    REQUIRE(started.has_value());

    auto missing_reply = co_await adapter.send(channel::OutboundMessage{
        .conversation_id = "c2c:user-openid",
        .content = {core::TextContent{.text = "hello"}},
        .reactions = {},
    });
    REQUIRE_FALSE(missing_reply.has_value());
    REQUIRE(missing_reply.error().kind() == core::ErrorKind::invalid_argument);

    auto bad_conversation = co_await adapter.send(channel::OutboundMessage{
        .conversation_id = "guild:channel-id",
        .content = {core::TextContent{.text = "hello"}},
        .reply_to_message_id = "msg-1",
        .reactions = {},
    });
    REQUIRE_FALSE(bad_conversation.has_value());
    REQUIRE(bad_conversation.error().kind() == core::ErrorKind::invalid_argument);

    auto empty_text = co_await adapter.send(channel::OutboundMessage{
        .conversation_id = "group:group-openid",
        .content = {},
        .reply_to_message_id = "msg-1",
        .reactions = {},
    });
    REQUIRE_FALSE(empty_text.has_value());
    REQUIRE(empty_text.error().kind() == core::ErrorKind::invalid_argument);
  });

  REQUIRE(server.served_count() == 0);
  fixture.blocking.join();
}

TEST_CASE("QqChannel send rejects malformed receipt bodies", "[unit][channel-qq][channel][async]") {
  test::ScriptedHttpServer server{{token_ok("tok-1"), api_response(200, R"({"ok":true})")}};
  SendFixture fixture{server};
  qq::QqChannel adapter{std::move(fixture.transport), fixture.api, qq::QqChannelOptions{.id = "qq-main"}};

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto started = co_await adapter.start();
    REQUIRE(started.has_value());

    auto receipt = co_await adapter.send(channel::OutboundMessage{
        .conversation_id = "c2c:user-openid",
        .content = {core::TextContent{.text = "hello"}},
        .reply_to_message_id = "msg-1",
        .reactions = {},
    });
    REQUIRE_FALSE(receipt.has_value());
    REQUIRE(receipt.error().kind() == core::ErrorKind::parsing);
  });

  REQUIRE(server.served_count() == 2);
  fixture.blocking.join();
}

TEST_CASE("QqChannel next_message normalizes gateway dispatches", "[unit][channel-qq][channel][gateway][async]") {
  test::ScriptedHttpServer token_server{{token_ok("tok-1")}};
  ws::ScriptedWsServer gateway_server{{{
      ws::SendText{hello_frame(200ms)},
      ws::RecvFrame{},
      ws::SendText{ready_frame(1, "sess-1")},
      ws::SendText{dispatch_frame(2, "INTERACTION_CREATE", json{{"id", "interaction-1"}})},
      ws::SendText{dispatch_frame(3,
                                  "GROUP_AT_MESSAGE_CREATE",
                                  json{{"id", "msg-2"},
                                       {"group_openid", "group-openid"},
                                       {"content", "<@bot-openid> gateway hello"},
                                       {"author", {{"member_openid", "member-openid"}}}})},
  }}};
  ChannelFixture fixture{token_server, gateway_server};
  qq::QqChannel adapter{std::move(fixture.transport), fixture.api, qq::QqChannelOptions{.id = "qq-live"}};

  test::run_async(
      [&](asio::io_context&) -> async::Awaitable<void> {
        auto started = co_await adapter.start();
        REQUIRE(started.has_value());

        auto message = co_await adapter.next_message();
        REQUIRE(message.has_value());
        REQUIRE(message->channel_id == "qq-live");
        REQUIRE(message->conversation_id == "group:group-openid");
        REQUIRE(message->user_id == "member-openid");
        REQUIRE(message->origin == channel::Origin{.kind = "channel", .source = "qq"});
        REQUIRE(message->replies_to.size() == 1);
        REQUIRE(message->replies_to.front().message_id == "msg-2");
        REQUIRE(message->content.size() == 1);
        REQUIRE(core::text_view(message->content.front()).value_or("") == "gateway hello");
      },
      3s);

  const auto frames = gateway_server.recorded_frames();
  REQUIRE(frames.size() == 1);
  REQUIRE(json::parse(frames[0].payload).at("op") == 2);
  fixture.blocking.join();
}
