// src/oran-hook/in_process_sink.cpp — std::function-backed sink.

#include <oran/hook/in_process_sink.hpp>

#include <expected>
#include <utility>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/error.hpp>
#include <oran/core/result.hpp>
#include <oran/hook/decision.hpp>
#include <oran/hook/event.hpp>
#include <oran/hook/payload.hpp>
#include <oran/hook/sink.hpp>

namespace orangutan::hook {

async::Awaitable<core::Result<void>> InProcessSink::receive(Event event, PayloadPtr payload) {
  if (!callback_) {
    co_return std::unexpected(core::Error::invalid_argument("InProcessSink callback is empty").with("sink_id", id_));
  }
  co_return co_await callback_(event, std::move(payload));
}

async::Awaitable<core::Result<HookDecision>> InProcessSink::handle_blocking(Event event, PayloadPtr payload) {
  if (!blocking_callback_) {
    co_return co_await Sink::handle_blocking(event, std::move(payload));
  }
  co_return co_await blocking_callback_(event, std::move(payload));
}

}  // namespace orangutan::hook
