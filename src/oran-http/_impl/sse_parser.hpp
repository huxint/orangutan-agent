// src/oran-http/_impl/sse_parser.hpp - incremental Server-Sent Events parser.

#pragma once

#include <string>
#include <string_view>

#include <oran/http/client.hpp>

namespace orangutan::http::detail {

/// Incremental parser for the `text/event-stream` wire grammar. Feed byte
/// chunks as they arrive; each complete event (terminated by a blank line) is
/// reported through the caller's `on_event`. The parser holds partial lines and
/// partial events across `feed` calls so chunk boundaries are transparent.
class SseParser {
public:
  template <typename OnEvent>
  void feed(std::string_view chunk, OnEvent&& on_event) {
    for (const char byte : chunk) {
      if (byte != '\n') {
        pending_line_.push_back(byte);
        continue;
      }
      auto line = std::string_view{pending_line_};
      if (line.ends_with('\r')) {
        line.remove_suffix(1);
      }
      if (line.empty()) {
        dispatch(on_event);
      } else {
        consume_field(line);
      }
      pending_line_.clear();
    }
  }

private:
  void consume_field(std::string_view line) {
    if (line.front() == ':') {
      return;  // Comment line.
    }
    auto field = line;
    std::string_view value{};
    if (const auto colon = line.find(':'); colon != std::string_view::npos) {
      field = line.substr(0, colon);
      value = line.substr(colon + 1);
      if (value.starts_with(' ')) {
        value.remove_prefix(1);
      }
    }
    if (field == "event") {
      event_type_.assign(value);
    } else if (field == "data") {
      data_.append(value);
      data_.push_back('\n');
    }
    // `id`, `retry`, and unknown fields are intentionally ignored.
  }

  template <typename OnEvent>
  void dispatch(OnEvent&& on_event) {
    if (data_.empty()) {
      event_type_.clear();  // An event with no data field is not dispatched.
      return;
    }
    if (data_.back() == '\n') {
      data_.pop_back();
    }
    on_event(SseEvent{.event = event_type_.empty() ? std::string{"message"} : event_type_, .data = data_});
    event_type_.clear();
    data_.clear();
  }

  std::string pending_line_;
  std::string event_type_;
  std::string data_;
};

}  // namespace orangutan::http::detail
