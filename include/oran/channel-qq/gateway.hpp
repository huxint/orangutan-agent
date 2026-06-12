// include/oran/channel-qq/gateway.hpp — QQ WebSocket-gateway protocol state machine.
//
// Pure, offline-testable half of the gateway receive transport (milestone 2a):
// it decodes one gateway text frame at a time, advances the session state
// machine (Hello → heartbeat interval, Identify-vs-Resume, Dispatch →
// READY/RESUMED capture, Reconnect/Invalid-Session), and reports what the
// transport owner must do next as a `GatewayReaction`. The persistent `wss://`
// connection, the heartbeat timer, and reconnect scheduling are the network
// half (milestone 2b) and live behind the future `oran-http` WebSocket seam.
//
// Frames and outbound payloads cross this boundary as serialized JSON strings
// (mirroring `ApiClient` and `tool::Output::data_json`), so the public header
// stays free of JSON parser types (critical rule C6). The session never holds
// a secret: the auth token is injected into `build_identify`/`build_resume`
// (the `TokenStore` owns it), keeping C5 honest.
//
// Opcode / close-code / intent facts are code-grounded against Tencent's botgo
// and botpy SDKs; see docs/references/messaging-platform-apis.md §2.2.

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <oran/core/result.hpp>

namespace orangutan::channel::qq {

/// QQ gateway opcodes (the `op` field). v2 group/C2C bots use this exact set;
/// op 12/13 (HTTP callback ACK / URL validation) belong to the webhook
/// transport and never appear on the WebSocket gateway.
enum class GatewayOpcode : std::uint8_t {
  dispatch = 0,         // recv: carries `s` (seq), `t` (event type), `d`
  heartbeat = 1,        // send/recv: payload `d` is the last seq (or null)
  identify = 2,         // send: first authentication after Hello
  resume = 6,           // send: resume a dropped session
  reconnect = 7,        // recv: server asks us to reconnect + Resume
  invalid_session = 9,  // recv: `d` is a bare bool; false → drop session
  hello = 10,           // recv: first frame; `d.heartbeat_interval` in ms
  heartbeat_ack = 11,   // recv: acknowledges our heartbeat
};

/// One decoded gateway dispatch (op 0) that carries a non-lifecycle event for
/// the inbound message parser. READY / RESUMED are consumed internally to
/// update session state and do *not* surface as dispatches.
struct GatewayDispatch {
  std::string event_type;  // the `t` field: GROUP_AT_MESSAGE_CREATE, C2C_MESSAGE_CREATE, …
  std::string data_json;   // the `d` object, re-serialized for the inbound parser

  friend bool operator==(const GatewayDispatch&, const GatewayDispatch&) = default;
};

/// What the transport owner must do in response to one consumed frame. A single
/// frame can ask for several things at once (Hello → arm the heartbeat timer
/// *and* send Identify/Resume), so this is a struct of independent directives,
/// all empty by default. An unrecognized opcode yields an all-empty reaction.
struct GatewayReaction {
  /// Which outbound payload to send next. Build the bytes with the matching
  /// `GatewaySession::build_*`.
  enum class Send : std::uint8_t {
    none,
    identify,
    resume,
    heartbeat
  };

  /// How the connection must be re-established.
  enum class Reconnect : std::uint8_t {
    none,
    resume,
    fresh
  };

  /// op 10 Hello: (re)arm the heartbeat timer with this interval.
  std::optional<std::chrono::milliseconds> heartbeat_interval{};
  Send send{Send::none};
  /// op 0 non-lifecycle dispatch to hand to the inbound parser.
  std::optional<GatewayDispatch> dispatch{};
  Reconnect reconnect{Reconnect::none};
  /// READY or RESUMED just arrived — the session is live and able to send.
  bool session_ready{false};

  friend bool operator==(const GatewayReaction&, const GatewayReaction&) = default;
};

/// Recovery decision for a WebSocket close code. Code-grounded against
/// `botgo/errs/err.go` (the wiki page 404s); see the reference §2.2.
enum class CloseRecovery : std::uint8_t {
  resume,          // 4009 only — the one resume-able unexpected close.
  fresh_identify,  // 4006/4007 + any other non-fatal close: drop session, re-Identify.
  refresh_token,   // 4004 auth failed: refresh the token, then reconnect fresh.
  fix_config,      // 4010–4014: bad shard / api version / invalid|unauthorized intents.
  fatal,           // 4914 offline / 4915 banned — do not reconnect.
};

/// Classify a WebSocket close code into the recovery action the transport owner
/// should take. A normal close (1000) and any code we do not recognize map to
/// `fresh_identify`: reconnect without assuming the session survived.
[[nodiscard]] CloseRecovery classify_close_code(std::uint16_t code) noexcept;

struct GatewaySessionOptions {
  /// Gateway intents bitmask. Default = GROUP_AND_C2C (1<<25) | INTERACTION
  /// (1<<26): the single bit that gates both group and C2C private messages,
  /// plus button/inline-keyboard callbacks. An unauthorized intent closes the
  /// socket with 4014 — keep this to bits the bot is actually granted.
  std::uint32_t intents{(std::uint32_t{1} << 25) | (std::uint32_t{1} << 26)};
  int shard_id{0};
  int shard_count{1};
  /// Heartbeat interval used until Hello supplies the authoritative value.
  std::chrono::milliseconds default_heartbeat_interval{41'250};
};

/// Decodes gateway frames and tracks session continuity. One instance per
/// logical gateway connection; it survives reconnects so a Resume can replay
/// missed events (persist `session_id()` / `last_seq()` to survive restarts).
///
/// Not thread-safe by design: the transport owner drives `consume` from a
/// single coroutine, the same one-resume-per-call discipline the `Channel`
/// trait expects.
class GatewaySession {
public:
  explicit GatewaySession(GatewaySessionOptions options = {});

  /// Decode one gateway text frame and advance the state machine. Returns a
  /// `parsing` error only when `frame_json` is not a JSON object; an unknown
  /// or absent opcode yields an empty reaction (logged-and-ignored upstream).
  [[nodiscard]] core::Result<GatewayReaction> consume(std::string_view frame_json);

  /// Build the outbound payload for the matching `GatewayReaction::Send`. The
  /// token is injected here so the session never stores the secret; pass the
  /// raw access token (without the `QQBot ` prefix — `build_*` adds it).
  [[nodiscard]] std::string build_identify(std::string_view token) const;
  [[nodiscard]] std::string build_resume(std::string_view token) const;
  [[nodiscard]] std::string build_heartbeat() const;

  /// Whether the next connect should Resume (a session id and a non-zero seq
  /// are both present) rather than start fresh with Identify.
  [[nodiscard]] bool can_resume() const noexcept;

  /// Session continuity, for persistence across process restarts.
  [[nodiscard]] std::string_view session_id() const noexcept;
  [[nodiscard]] std::uint32_t last_seq() const noexcept;
  /// Re-seat a persisted session before the first connect so Hello picks Resume.
  void restore(std::string session_id, std::uint32_t last_seq);
  /// Drop session continuity (op 9 invalid-session, or an authoritative reset).
  void reset() noexcept;

  /// The heartbeat interval Hello supplied, or the configured default until then.
  [[nodiscard]] std::chrono::milliseconds heartbeat_interval() const noexcept;

private:
  GatewaySessionOptions options_;
  std::string session_id_;
  std::uint32_t last_seq_{0};
  std::chrono::milliseconds heartbeat_interval_;
};

}  // namespace orangutan::channel::qq
