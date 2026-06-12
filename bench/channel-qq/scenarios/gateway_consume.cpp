// bench/channel-qq/scenarios/gateway_consume.cpp
//
// A-vs-B comparison of `GatewaySession::consume`: a heartbeat-ack frame (op
// decode + empty reaction, the no-op hot path the read loop sees most) vs. a
// dispatch frame carrying a group message (op decode + seq cache + `d`
// re-serialization for the inbound parser). This prices the per-frame protocol
// cost the gateway read loop pays on top of the WebSocket transport, and
// justifies returning lifecycle frames before the dispatch re-serialization.

#include <nanobench.h>

#include <string>

#include <nlohmann/json.hpp>

#include <oran/channel-qq/gateway.hpp>

namespace orangutan::bench {

namespace {

namespace qq = orangutan::channel::qq;

const std::string kHeartbeatAck = nlohmann::json{{"op", 11}}.dump();
const std::string kDispatch =
    nlohmann::json{
        {"op", 0},
        {"s", 4096},
        {"t", "GROUP_AT_MESSAGE_CREATE"},
        {"d", {{"content", "hello there"}, {"group_openid", "g-1"}, {"id", "msg-1"}}},
    }
        .dump();

}  // namespace

void register_channel_qq_gateway_consume(ankerl::nanobench::Bench& b) {
  b.run("gateway consume: heartbeat-ack (decode + no-op)", [&] {
    qq::GatewaySession session;
    auto reaction = session.consume(kHeartbeatAck);
    ankerl::nanobench::doNotOptimizeAway(reaction);
  });

  b.run("gateway consume: dispatch (decode + reserialize d)", [&] {
    qq::GatewaySession session;
    auto reaction = session.consume(kDispatch);
    ankerl::nanobench::doNotOptimizeAway(reaction);
  });
}

}  // namespace orangutan::bench
