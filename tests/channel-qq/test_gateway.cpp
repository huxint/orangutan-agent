// tests/channel-qq/test_gateway.cpp — QQ gateway protocol/session state machine.
//
// Pure, offline coverage of the milestone-2a `GatewaySession`: opcode decoding,
// the Identify-vs-Resume decision, READY/RESUMED session capture, seq tracking,
// heartbeat-payload building, close-code classification, and the
// invalid-session / reconnect directives. No network — `consume` is fed raw
// gateway JSON frames exactly as the transport owner will hand them over.

#include <oran/channel-qq/gateway.hpp>

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/error.hpp>

using namespace std::chrono_literals;

namespace {

namespace core = orangutan::core;
namespace qq = orangutan::channel::qq;

using json = nlohmann::json;

[[nodiscard]] std::string hello_frame(std::int64_t interval_ms) {
  return json{{"op", 10}, {"d", {{"heartbeat_interval", interval_ms}}}}.dump();
}

[[nodiscard]] std::string dispatch_frame(std::uint32_t seq, std::string event_type, json data) {
  return json{{"op", 0}, {"s", seq}, {"t", std::move(event_type)}, {"d", std::move(data)}}.dump();
}

}  // namespace

TEST_CASE("gateway Hello arms the heartbeat and triggers Identify when fresh", "[unit][channel-qq][gateway]") {
  qq::GatewaySession session;

  auto reaction = session.consume(hello_frame(30'000));
  REQUIRE(reaction.has_value());
  REQUIRE(reaction->heartbeat_interval == 30'000ms);
  REQUIRE(reaction->send == qq::GatewayReaction::Send::identify);
  REQUIRE(session.heartbeat_interval() == 30'000ms);
  REQUIRE_FALSE(session.can_resume());
}

TEST_CASE("gateway Hello honors a string heartbeat interval", "[unit][channel-qq][gateway]") {
  qq::GatewaySession session;

  // QQ sends several numeric fields as strings depending on SDK/version.
  auto reaction = session.consume(json{{"op", 10}, {"d", {{"heartbeat_interval", "45000"}}}}.dump());
  REQUIRE(reaction.has_value());
  REQUIRE(reaction->heartbeat_interval == 45'000ms);
}

TEST_CASE("gateway Hello falls back to the default interval when absent", "[unit][channel-qq][gateway]") {
  qq::GatewaySession session{qq::GatewaySessionOptions{.default_heartbeat_interval = 41'250ms}};

  auto reaction = session.consume(json{{"op", 10}, {"d", json::object()}}.dump());
  REQUIRE(reaction.has_value());
  REQUIRE(reaction->heartbeat_interval == 41'250ms);
}

TEST_CASE("gateway READY captures the session id and reports ready", "[unit][channel-qq][gateway]") {
  qq::GatewaySession session;

  auto reaction = session.consume(dispatch_frame(1, "READY", json{{"session_id", "sess-abc"}}));
  REQUIRE(reaction.has_value());
  REQUIRE(reaction->session_ready);
  REQUIRE_FALSE(reaction->dispatch.has_value());
  REQUIRE(session.session_id() == "sess-abc");
  REQUIRE(session.last_seq() == 1);
  REQUIRE(session.can_resume());
}

TEST_CASE("gateway surfaces non-lifecycle dispatches to the inbound parser", "[unit][channel-qq][gateway]") {
  qq::GatewaySession session;

  auto reaction =
      session.consume(dispatch_frame(7, "GROUP_AT_MESSAGE_CREATE", json{{"content", "hi"}, {"group_openid", "g-1"}}));
  REQUIRE(reaction.has_value());
  REQUIRE_FALSE(reaction->session_ready);
  REQUIRE(reaction->dispatch.has_value());
  REQUIRE(reaction->dispatch->event_type == "GROUP_AT_MESSAGE_CREATE");

  // The `d` object is re-serialized verbatim for the milestone-3 inbound parser.
  const auto data = json::parse(reaction->dispatch->data_json);
  REQUIRE(data.at("content") == "hi");
  REQUIRE(data.at("group_openid") == "g-1");
  REQUIRE(session.last_seq() == 7);
}

TEST_CASE("gateway tracks the highest seq across dispatches", "[unit][channel-qq][gateway]") {
  qq::GatewaySession session;

  static_cast<void>(session.consume(dispatch_frame(3, "C2C_MESSAGE_CREATE", json::object())));
  REQUIRE(session.last_seq() == 3);
  static_cast<void>(session.consume(dispatch_frame(4, "C2C_MESSAGE_CREATE", json::object())));
  REQUIRE(session.last_seq() == 4);
}

TEST_CASE("gateway picks Resume after a session is established", "[unit][channel-qq][gateway]") {
  qq::GatewaySession session;
  static_cast<void>(session.consume(dispatch_frame(5, "READY", json{{"session_id", "sess-1"}})));

  // A later Hello (after a drop/reconnect) must Resume, not Identify.
  auto reaction = session.consume(hello_frame(30'000));
  REQUIRE(reaction.has_value());
  REQUIRE(reaction->send == qq::GatewayReaction::Send::resume);
}

TEST_CASE("gateway restore re-seats a persisted session for Resume", "[unit][channel-qq][gateway]") {
  qq::GatewaySession session;
  session.restore("sess-persisted", 99);
  REQUIRE(session.can_resume());

  auto reaction = session.consume(hello_frame(30'000));
  REQUIRE(reaction.has_value());
  REQUIRE(reaction->send == qq::GatewayReaction::Send::resume);
}

TEST_CASE("gateway RESUMED reports ready without clearing continuity", "[unit][channel-qq][gateway]") {
  qq::GatewaySession session;
  session.restore("sess-1", 42);

  auto reaction = session.consume(json{{"op", 0}, {"t", "RESUMED"}, {"d", json::object()}}.dump());
  REQUIRE(reaction.has_value());
  REQUIRE(reaction->session_ready);
  REQUIRE(session.session_id() == "sess-1");
  REQUIRE(session.last_seq() == 42);
}

TEST_CASE("gateway reconnect opcode preserves the session", "[unit][channel-qq][gateway]") {
  qq::GatewaySession session;
  session.restore("sess-1", 10);

  auto reaction = session.consume(json{{"op", 7}}.dump());
  REQUIRE(reaction.has_value());
  REQUIRE(reaction->reconnect == qq::GatewayReaction::Reconnect::resume);
  REQUIRE(session.can_resume());
}

TEST_CASE("gateway invalid-session drops continuity when not resumable", "[unit][channel-qq][gateway]") {
  qq::GatewaySession session;
  session.restore("sess-1", 10);

  auto reaction = session.consume(json{{"op", 9}, {"d", false}}.dump());
  REQUIRE(reaction.has_value());
  REQUIRE(reaction->reconnect == qq::GatewayReaction::Reconnect::fresh);
  REQUIRE_FALSE(session.can_resume());
  REQUIRE(session.session_id().empty());
  REQUIRE(session.last_seq() == 0);
}

TEST_CASE("gateway invalid-session keeps continuity when resumable", "[unit][channel-qq][gateway]") {
  qq::GatewaySession session;
  session.restore("sess-1", 10);

  auto reaction = session.consume(json{{"op", 9}, {"d", true}}.dump());
  REQUIRE(reaction.has_value());
  REQUIRE(reaction->reconnect == qq::GatewayReaction::Reconnect::resume);
  REQUIRE(session.can_resume());
}

TEST_CASE("gateway solicited heartbeat asks us to send one", "[unit][channel-qq][gateway]") {
  qq::GatewaySession session;

  auto reaction = session.consume(json{{"op", 1}, {"d", nullptr}}.dump());
  REQUIRE(reaction.has_value());
  REQUIRE(reaction->send == qq::GatewayReaction::Send::heartbeat);
}

TEST_CASE("gateway heartbeat-ack and unknown opcodes yield empty reactions", "[unit][channel-qq][gateway]") {
  qq::GatewaySession session;

  auto ack = session.consume(json{{"op", 11}}.dump());
  REQUIRE(ack.has_value());
  REQUIRE(*ack == qq::GatewayReaction{});

  auto unknown = session.consume(json{{"op", 99}}.dump());
  REQUIRE(unknown.has_value());
  REQUIRE(*unknown == qq::GatewayReaction{});

  auto opless = session.consume(json{{"d", json::object()}}.dump());
  REQUIRE(opless.has_value());
  REQUIRE(*opless == qq::GatewayReaction{});
}

TEST_CASE("gateway rejects a non-object frame", "[unit][channel-qq][gateway]") {
  qq::GatewaySession session;

  auto array_frame = session.consume("[1,2,3]");
  REQUIRE_FALSE(array_frame.has_value());
  REQUIRE(array_frame.error().kind() == core::ErrorKind::parsing);

  auto garbage = session.consume("not json");
  REQUIRE_FALSE(garbage.has_value());
  REQUIRE(garbage.error().kind() == core::ErrorKind::parsing);
}

TEST_CASE("gateway builds an Identify payload with auth, intents, and shard", "[unit][channel-qq][gateway]") {
  qq::GatewaySession session{qq::GatewaySessionOptions{
      .intents = (std::uint32_t{1} << 25) | (std::uint32_t{1} << 26),
      .shard_id = 0,
      .shard_count = 1,
  }};

  const auto payload = json::parse(session.build_identify("tok-xyz"));
  REQUIRE(payload.at("op") == 2);
  REQUIRE(payload.at("d").at("token") == "QQBot tok-xyz");
  REQUIRE(payload.at("d").at("intents") == 100663296U);  // 1<<25 | 1<<26
  REQUIRE(payload.at("d").at("shard") == json::array({0, 1}));
  REQUIRE(payload.at("d").at("properties").at("$browser") == "orangutan");
}

TEST_CASE("gateway builds a Resume payload from session continuity", "[unit][channel-qq][gateway]") {
  qq::GatewaySession session;
  session.restore("sess-1", 77);

  const auto payload = json::parse(session.build_resume("tok-xyz"));
  REQUIRE(payload.at("op") == 6);
  REQUIRE(payload.at("d").at("token") == "QQBot tok-xyz");
  REQUIRE(payload.at("d").at("session_id") == "sess-1");
  REQUIRE(payload.at("d").at("seq") == 77);
}

TEST_CASE("gateway heartbeat payload carries the last seq or null", "[unit][channel-qq][gateway]") {
  qq::GatewaySession session;

  const auto before = json::parse(session.build_heartbeat());
  REQUIRE(before.at("op") == 1);
  REQUIRE(before.at("d").is_null());

  static_cast<void>(session.consume(dispatch_frame(12, "C2C_MESSAGE_CREATE", json::object())));
  const auto after = json::parse(session.build_heartbeat());
  REQUIRE(after.at("d") == 12);
}

TEST_CASE("classify_close_code maps codes to the corrected recovery actions", "[unit][channel-qq][gateway]") {
  // Resume-able: 4009 is the *only* unexpected close that preserves the session.
  REQUIRE(qq::classify_close_code(4009) == qq::CloseRecovery::resume);

  // Auth: refresh the token, then reconnect.
  REQUIRE(qq::classify_close_code(4004) == qq::CloseRecovery::refresh_token);

  // Config: bad shard / api version / invalid|unauthorized intents (4013/4014).
  REQUIRE(qq::classify_close_code(4010) == qq::CloseRecovery::fix_config);
  REQUIRE(qq::classify_close_code(4013) == qq::CloseRecovery::fix_config);
  REQUIRE(qq::classify_close_code(4014) == qq::CloseRecovery::fix_config);

  // Fatal: 4914 offline / 4915 banned — no reconnect. (NOT invalid intents:
  // the doc-only research pass got this wrong; botgo source is authoritative.)
  REQUIRE(qq::classify_close_code(4914) == qq::CloseRecovery::fatal);
  REQUIRE(qq::classify_close_code(4915) == qq::CloseRecovery::fatal);

  // Fresh Identify: normal close, session-no-longer-valid, invalid-seq,
  // rate-limited, and any unrecognized code.
  REQUIRE(qq::classify_close_code(1000) == qq::CloseRecovery::fresh_identify);
  REQUIRE(qq::classify_close_code(4006) == qq::CloseRecovery::fresh_identify);
  REQUIRE(qq::classify_close_code(4007) == qq::CloseRecovery::fresh_identify);
  REQUIRE(qq::classify_close_code(4008) == qq::CloseRecovery::fresh_identify);
  REQUIRE(qq::classify_close_code(3000) == qq::CloseRecovery::fresh_identify);
}
