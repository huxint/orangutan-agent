// src/oran-hook/in_process_sink.cpp — std::function-backed sink.

#include <oran/hook/in_process_sink.hpp>

#include <expected>
#include <utility>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/error.hpp>
#include <oran/core/result.hpp>
#include <oran/hook/event.hpp>
#include <oran/hook/payload.hpp>

namespace orangutan::hook {

async::Awaitable<core::Result<void>> InProcessSink::receive(Event event, Payload payload) {
  if (!callback_) {
    co_return std::unexpected(core::Error::invalid_argument("InProcessSink callback is empty").with("sink_id", id_));
  }
  co_return co_await callback_(event, std::move(payload));
}

}  // namespace orangutan::hook
