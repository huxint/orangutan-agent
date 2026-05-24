// src/oran-hook/sink.cpp — `hook::Sink` default implementations.
//
// `handle_blocking` defaults to `proceed` so sinks that only care about
// advisory events do not have to override it. Coroutine bodies live here
// rather than in the public header to keep the per-TU compile cost in
// the consumer libraries bounded (rule C6 / FAST_COMPILATION).

#include <oran/hook/sink.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/hook/decision.hpp>
#include <oran/hook/event.hpp>
#include <oran/hook/payload.hpp>

namespace orangutan::hook {

async::Awaitable<core::Result<HookDecision>> Sink::handle_blocking(Event /*event*/, Payload /*payload*/) {
  co_return HookDecision{};
}

}  // namespace orangutan::hook
