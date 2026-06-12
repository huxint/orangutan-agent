// src/oran-channel-qq/gateway.cpp — QQ gateway protocol/session state machine.

#include <oran/channel-qq/gateway.hpp>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include <oran/core/error.hpp>

namespace orangutan::channel::qq {

namespace {

/// QQ's gateway, like several of its JSON fields, sends `heartbeat_interval`
/// as either a number or a decimal string depending on SDK/version. Decode
/// both; fall back to `fallback` for anything missing or unparseable.
[[nodiscard]] std::int64_t
parse_integer_like(const nlohmann::json& object, std::string_view key, std::int64_t fallback) noexcept {
  const auto field = object.find(key);
  if (field == object.end()) {
    return fallback;
  }
  if (field->is_number_integer()) {
    return field->get<std::int64_t>();
  }
  if (field->is_string()) {
    const auto& text = field->get_ref<const std::string&>();
    std::int64_t value = 0;
    const auto* first = text.data();
    const auto* last = first + text.size();
    if (std::from_chars(first, last, value).ec == std::errc{}) {
      return value;
    }
  }
  return fallback;
}

[[nodiscard]] std::string auth_field(std::string_view token) {
  return "QQBot " + std::string{token};
}

}  // namespace

CloseRecovery classify_close_code(std::uint16_t code) noexcept {
  switch (code) {
    case 4004:
      return CloseRecovery::refresh_token;
    case 4009:
      // The one unexpected close that preserves the session — Resume is valid.
      return CloseRecovery::resume;
    case 4010:
    case 4011:
    case 4012:
    case 4013:  // invalid intents
    case 4014:  // unauthorized intents
      return CloseRecovery::fix_config;
    case 4914:  // bot offline / delisted
    case 4915:  // bot banned
      return CloseRecovery::fatal;
    default:
      // 1000 normal, 4006 session-no-longer-valid, 4007 invalid-seq, 4008
      // rate-limited, and any unrecognized code: reconnect but do not assume
      // the session survived.
      return CloseRecovery::fresh_identify;
  }
}

GatewaySession::GatewaySession(GatewaySessionOptions options)
    : options_{std::move(options)}, heartbeat_interval_{options_.default_heartbeat_interval} {}

core::Result<GatewayReaction> GatewaySession::consume(std::string_view frame_json) {
  const auto frame = nlohmann::json::parse(frame_json, nullptr, /*allow_exceptions=*/false);
  if (!frame.is_object()) {
    return std::unexpected(core::Error::parsing("qq gateway frame is not a JSON object"));
  }

  // Cache the dispatch sequence (`s`) whenever the frame carries one; it is the
  // resume cursor and the heartbeat payload. Absent or null on lifecycle frames.
  if (const auto seq = frame.find("s"); seq != frame.end() && seq->is_number_unsigned()) {
    last_seq_ = seq->get<std::uint32_t>();
  }

  auto reaction = GatewayReaction{};
  const auto op = frame.value("op", -1);
  switch (op) {
    case static_cast<int>(GatewayOpcode::hello): {
      const auto data = frame.value("d", nlohmann::json::object());
      heartbeat_interval_ = std::chrono::milliseconds{
          parse_integer_like(data, "heartbeat_interval", options_.default_heartbeat_interval.count())};
      reaction.heartbeat_interval = heartbeat_interval_;
      reaction.send = can_resume() ? GatewayReaction::Send::resume : GatewayReaction::Send::identify;
      return reaction;
    }
    case static_cast<int>(GatewayOpcode::dispatch): {
      auto event_type = frame.value("t", std::string{});
      const auto data = frame.value("d", nlohmann::json::object());
      if (event_type == "READY") {
        session_id_ = data.value("session_id", std::string{});
        reaction.session_ready = true;
        return reaction;
      }
      if (event_type == "RESUMED") {
        reaction.session_ready = true;
        return reaction;
      }
      reaction.dispatch = GatewayDispatch{.event_type = std::move(event_type), .data_json = data.dump()};
      return reaction;
    }
    case static_cast<int>(GatewayOpcode::heartbeat):
      // Server-requested immediate heartbeat (the gateway can solicit one).
      reaction.send = GatewayReaction::Send::heartbeat;
      return reaction;
    case static_cast<int>(GatewayOpcode::heartbeat_ack):
      return reaction;
    case static_cast<int>(GatewayOpcode::reconnect):
      // The server preserves the session across a requested reconnect.
      reaction.reconnect = GatewayReaction::Reconnect::resume;
      return reaction;
    case static_cast<int>(GatewayOpcode::invalid_session): {
      // `d` is a bare boolean: true means the session may still be resumed,
      // false means drop it and re-Identify.
      const auto resumable = frame.value("d", false);
      if (resumable) {
        reaction.reconnect = GatewayReaction::Reconnect::resume;
      } else {
        reset();
        reaction.reconnect = GatewayReaction::Reconnect::fresh;
      }
      return reaction;
    }
    default:
      // Unknown / absent opcode: empty reaction (logged-and-ignored upstream).
      return reaction;
  }
}

std::string GatewaySession::build_identify(std::string_view token) const {
  const auto payload = nlohmann::json{
      {"op", static_cast<int>(GatewayOpcode::identify)},
      {"d",
       {
           {"token", auth_field(token)},
           {"intents", options_.intents},
           {"shard", nlohmann::json::array({options_.shard_id, options_.shard_count})},
           {"properties", {{"$os", "linux"}, {"$browser", "orangutan"}, {"$device", "orangutan"}}},
       }},
  };
  return payload.dump();
}

std::string GatewaySession::build_resume(std::string_view token) const {
  const auto payload = nlohmann::json{
      {"op", static_cast<int>(GatewayOpcode::resume)},
      {"d", {{"token", auth_field(token)}, {"session_id", session_id_}, {"seq", last_seq_}}},
  };
  return payload.dump();
}

std::string GatewaySession::build_heartbeat() const {
  auto payload = nlohmann::json{{"op", static_cast<int>(GatewayOpcode::heartbeat)}};
  // The heartbeat `d` is the last seq, or null before the first dispatch.
  if (last_seq_ == 0) {
    payload["d"] = nullptr;
  } else {
    payload["d"] = last_seq_;
  }
  return payload.dump();
}

bool GatewaySession::can_resume() const noexcept {
  return !session_id_.empty() && last_seq_ > 0;
}

std::string_view GatewaySession::session_id() const noexcept {
  return session_id_;
}

std::uint32_t GatewaySession::last_seq() const noexcept {
  return last_seq_;
}

void GatewaySession::restore(std::string session_id, std::uint32_t last_seq) {
  session_id_ = std::move(session_id);
  last_seq_ = last_seq;
}

void GatewaySession::reset() noexcept {
  session_id_.clear();
  last_seq_ = 0;
}

std::chrono::milliseconds GatewaySession::heartbeat_interval() const noexcept {
  return heartbeat_interval_;
}

}  // namespace orangutan::channel::qq
