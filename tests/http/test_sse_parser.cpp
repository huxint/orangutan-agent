// tests/http/test_sse_parser.cpp - incremental SSE wire parser coverage.
//
// The parser lives under src/oran-http/_impl/ so we reach it through a relative
// path; it is an internal compilation detail, not part of the public surface.

#include "../../src/oran-http/_impl/sse_parser.hpp"

#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <oran/http.hpp>

namespace {

namespace http = orangutan::http;

[[nodiscard]] std::vector<http::SseEvent> drain(http::detail::SseParser& parser, std::string_view chunk) {
  std::vector<http::SseEvent> events;
  parser.feed(chunk, [&](const http::SseEvent& event) { events.push_back(event); });
  return events;
}

}  // namespace

TEST_CASE("SseParser emits one default-type message event", "[unit][http][sse]") {
  http::detail::SseParser parser;
  const auto events = drain(parser, "data: hello\n\n");
  REQUIRE(events.size() == 1);
  REQUIRE(events[0] == http::SseEvent{.event = "message", .data = "hello"});
}

TEST_CASE("SseParser carries an explicit event type", "[unit][http][sse]") {
  http::detail::SseParser parser;
  const auto events = drain(parser, "event: message_start\ndata: {\"type\":\"message_start\"}\n\n");
  REQUIRE(events.size() == 1);
  REQUIRE(events[0] == http::SseEvent{.event = "message_start", .data = R"({"type":"message_start"})"});
}

TEST_CASE("SseParser joins multi-line data with newlines", "[unit][http][sse]") {
  http::detail::SseParser parser;
  const auto events = drain(parser, "data: first\ndata: second\n\n");
  REQUIRE(events.size() == 1);
  REQUIRE(events[0] == http::SseEvent{.event = "message", .data = "first\nsecond"});
}

TEST_CASE("SseParser strips at most one leading space after the colon", "[unit][http][sse]") {
  http::detail::SseParser parser;
  const auto events = drain(parser, "data:no-space\ndata:  one-extra\n\n");
  REQUIRE(events.size() == 1);
  REQUIRE(events[0] == http::SseEvent{.event = "message", .data = "no-space\n one-extra"});
}

TEST_CASE("SseParser accepts CRLF line endings", "[unit][http][sse]") {
  http::detail::SseParser parser;
  const auto events = drain(parser, "event: ping\r\ndata: {}\r\n\r\n");
  REQUIRE(events.size() == 1);
  REQUIRE(events[0] == http::SseEvent{.event = "ping", .data = "{}"});
}

TEST_CASE("SseParser ignores comment, id, and retry lines", "[unit][http][sse]") {
  http::detail::SseParser parser;
  const auto events = drain(parser, ": keep-alive\nid: 42\nretry: 1000\ndata: ok\n\n");
  REQUIRE(events.size() == 1);
  REQUIRE(events[0] == http::SseEvent{.event = "message", .data = "ok"});
}

TEST_CASE("SseParser reassembles an event split across chunks", "[unit][http][sse]") {
  http::detail::SseParser parser;
  auto events = drain(parser, "event: content_block_de");
  REQUIRE(events.empty());
  events = drain(parser, "lta\ndata: {\"i\":0}");
  REQUIRE(events.empty());
  events = drain(parser, "\n\n");
  REQUIRE(events.size() == 1);
  REQUIRE(events[0] == http::SseEvent{.event = "content_block_delta", .data = R"({"i":0})"});
}

TEST_CASE("SseParser does not dispatch an event that carries no data", "[unit][http][sse]") {
  http::detail::SseParser parser;
  auto events = drain(parser, "event: orphan\n\n");
  REQUIRE(events.empty());
  // The orphaned event type must not leak into the next event.
  events = drain(parser, "data: after\n\n");
  REQUIRE(events.size() == 1);
  REQUIRE(events[0] == http::SseEvent{.event = "message", .data = "after"});
}

TEST_CASE("SseParser dispatches multiple events from one chunk", "[unit][http][sse]") {
  http::detail::SseParser parser;
  const auto events = drain(parser, "data: 1\n\ndata: 2\n\n");
  REQUIRE(events.size() == 2);
  REQUIRE(events[0] == http::SseEvent{.event = "message", .data = "1"});
  REQUIRE(events[1] == http::SseEvent{.event = "message", .data = "2"});
}
