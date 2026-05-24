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
#include <oran/hook/decision.hpp>
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

  /// Decide how to handle a blocking event (`tool_before`,
  /// `permission_ask_rendered`, `memory_write_before` per spec 0015 v1).
  /// The bus calls this through `publish_blocking<E>` once per subscribed
  /// sink in subscription order and stops at the first non-`proceed`
  /// decision. The default implementation returns `HookDecision{}`
  /// (i.e. `kind = proceed`, empty `reason`) so sinks that only care
  /// about advisory events can ignore this method entirely; sinks that
  /// want to veto / rewrite / require_approval override it.
  ///
  /// Returning an error is treated as a veto by `publish_blocking`
  /// (`reason = hook_error`); a thrown exception is captured the same
  /// way. `tool::Registry::dispatch` is the first consumer for
  /// `tool_before`; `cli::OperatorPromptSink` is the first concrete
  /// terminal consumer for `permission_ask_rendered`.
  [[nodiscard]] virtual async::Awaitable<core::Result<HookDecision>> handle_blocking(Event event, Payload payload);
};

}  // namespace orangutan::hook
