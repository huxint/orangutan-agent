// include/oran/hook/in_process_sink.hpp — std::function-backed sink.
//
// The simplest sink: forwards every event to a `std::function` the caller
// supplies. Useful for tests, the agent loop's in-process subscribers, and
// any audit-style consumer that lives inside the binary.

#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/hook/event.hpp>
#include <oran/hook/payload.hpp>
#include <oran/hook/sink.hpp>

namespace orangutan::hook {

class InProcessSink final : public Sink {
public:
  using Callback = std::function<async::Awaitable<core::Result<void>>(Event, Payload)>;

  /// `id` is the stable sink identifier (see `Sink::id`); `callback` is
  /// invoked once per received event. The callback may return an error;
  /// the bus captures it in the publish outcome but other sinks still run.
  InProcessSink(std::string id, Callback callback) : id_(std::move(id)), callback_(std::move(callback)) {}

  [[nodiscard]] std::string_view id() const noexcept override {
    return id_;
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> receive(Event event, Payload payload) override;

private:
  std::string id_;
  Callback callback_;
};

}  // namespace orangutan::hook
