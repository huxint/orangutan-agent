// include/oran/hook/in_process_sink.hpp — std::function-backed sink.
//
// The simplest sink: forwards every event to a `std::function` the caller
// supplies. Useful for tests, the agent loop's in-process subscribers, and
// any audit-style consumer that lives inside the binary.
//
// An optional second callback handles spec-0015 blocking publishes; when
// it is unset the sink falls back to `Sink::handle_blocking`'s
// `proceed` default.

#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/hook/decision.hpp>
#include <oran/hook/event.hpp>
#include <oran/hook/payload.hpp>
#include <oran/hook/sink.hpp>

namespace orangutan::hook {

class InProcessSink final : public Sink {
public:
  using Callback = std::function<async::Awaitable<core::Result<void>>(Event, Payload)>;
  /// Optional blocking handler. Returning a non-`proceed` decision short-
  /// circuits the bus's per-event walk.
  using BlockingCallback = std::function<async::Awaitable<core::Result<HookDecision>>(Event, Payload)>;

  /// `id` is the stable sink identifier (see `Sink::id`); `callback` is
  /// invoked once per received event. The callback may return an error;
  /// the bus captures it in the publish outcome but other sinks still run.
  InProcessSink(std::string id, Callback callback, SinkKind kind = SinkKind::default_)
      : id_(std::move(id)), callback_(std::move(callback)), kind_(kind) {}

  /// Install (or replace) the blocking handler. Pass an empty
  /// `std::function` to remove a previously installed handler — the sink
  /// then reverts to the `Sink::handle_blocking` default.
  void set_blocking_handler(BlockingCallback handler) noexcept {
    blocking_callback_ = std::move(handler);
  }

  [[nodiscard]] std::string_view id() const noexcept override {
    return id_;
  }

  [[nodiscard]] SinkKind kind() const noexcept override {
    return kind_;
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> receive(Event event, Payload payload) override;

  [[nodiscard]] async::Awaitable<core::Result<HookDecision>> handle_blocking(Event event, Payload payload) override;

private:
  std::string id_;
  Callback callback_;
  BlockingCallback blocking_callback_;
  SinkKind kind_{SinkKind::default_};
};

}  // namespace orangutan::hook
