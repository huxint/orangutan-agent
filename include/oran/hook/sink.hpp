// include/oran/hook/sink.hpp — abstract hook sink.
//
// A sink receives one (`Event`, `Payload`) at a time and reports success or
// failure. Sinks are non-owning from the bus's point of view; the caller
// owns them (typically the bootstrap layer) and the bus keeps raw pointers.
//
// Concurrency. Sinks are not thread-safe. The bus dispatches on the agent's
// strand; sinks that need to publish elsewhere wrap themselves in an
// `asio::strand`.

#pragma once

#include <cstdint>
#include <string_view>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/hook/event.hpp>
#include <oran/hook/payload.hpp>

namespace orangutan::hook {

/// Trust classification for hook sinks. Default sinks receive redacted
/// payloads; trusted-local sinks are in-process consumers that may inspect
/// raw structured tool data.
enum class SinkKind : std::uint8_t {
  default_,
  trusted_local,
};

/// Abstract sink. Concrete sinks: `InProcessSink` (slice 22, std::function
/// callback) and the planned `ShellSink` / `WebhookSink` / `LuaSink`.
class Sink {
public:
  Sink() = default;
  virtual ~Sink() = default;

  Sink(const Sink&) = delete;
  Sink& operator=(const Sink&) = delete;
  Sink(Sink&&) = delete;
  Sink& operator=(Sink&&) = delete;

  /// Stable identifier for audit + filtering. The bus uses it to populate
  /// `PublishOutcome::SinkResult::sink_id`; the planned audit row stamps
  /// it on `hook_audit.sink_id`. Identifiers should be short and stable
  /// (e.g. `"shell-pre-tool"`, `"audit-webhook"`).
  [[nodiscard]] virtual std::string_view id() const noexcept = 0;

  /// Redaction policy for payload delivery. The default is conservative:
  /// external or unclassified sinks do not receive raw `ToolAfterPayload`
  /// structured output bytes.
  [[nodiscard]] virtual SinkKind kind() const noexcept {
    return SinkKind::default_;
  }

  /// Receive one event. The bus calls this once per published event the
  /// sink is subscribed to. Returning an error is captured in the
  /// publish outcome but does not abort the publish for other sinks
  /// (advisory contract).
  [[nodiscard]] virtual async::Awaitable<core::Result<void>> receive(Event event, Payload payload) = 0;
};

}  // namespace orangutan::hook
