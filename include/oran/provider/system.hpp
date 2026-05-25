// include/oran/provider/system.hpp — provider system surface.
//
// `System` is the runtime's single point of contact with whatever produces a
// `provider::Response` (a vendor adapter, a replay shim, the test fake). The
// agent loop builds a `Request`, picks a `Route`, optionally hands an
// `EventSink` for streaming deltas, and awaits the terminal `Response`.
//
// Spec 0017 freezes the v1 contract: exactly one `Request` per call, exactly
// one `Response` (or error) back. Stream deltas are advisory; if the caller
// passes `nullptr` the provider must still synthesise the full `Response`.
//
// The first concrete `System` is `provider::FakeProvider`
// (`<oran/provider/fake.hpp>`). Protocol-backed systems are constructed
// through adapter factories such as `ProtocolTransportAdapterFactory`; concrete
// HTTP/TLS transport and streaming remain downstream.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/core/stop_reason.hpp>
#include <oran/provider/cache.hpp>
#include <oran/provider/types.hpp>

namespace orangutan::provider {

/// Wire-format family for a vendor endpoint. The execution layer dispatches on
/// this enumerator (not on the provider name) so adding a new endpoint that
/// reuses an existing protocol is a config change, not code.
///
/// Wire spelling comes from `core::enum_name(value)` — no hand-maintained
/// string table per `docs/rules/code-style.md`.
enum class ProtocolKind : std::uint8_t {
  anthropic_messages,
  openai_chat_completions,
  openai_responses,
  gemini_generate_content,
  custom_openai_compatible,
};

/// A single (profile, model, protocol) tuple. Profiles live in config and name
/// the credential + base URL; the model is the vendor identifier; the protocol
/// is the wire family. The `oran-provider` library carries only enough of this
/// shape to let the loop pick between primary and fallback targets — heavier
/// fields (`Capabilities`, headers, cost) land with the first real adapter
/// slice.
struct ModelTarget {
  std::string profile;
  std::string model;
  ProtocolKind protocol{ProtocolKind::anthropic_messages};
  std::optional<std::uint32_t> thinking_budget;
  std::optional<PromptCacheOptions> cache;

  friend bool operator==(const ModelTarget&, const ModelTarget&) = default;
};

/// Resolved primary + fallback chain for one turn. Routes are resolved once
/// per turn by the agent loop and reused across iterations within that turn.
struct Route {
  ModelTarget primary;
  std::vector<ModelTarget> fallbacks;

  friend bool operator==(const Route&, const Route&) = default;
};

/// Streaming observer. Adapters call into the sink as deltas arrive on the
/// wire; the sink converts them into whatever surface the caller cares about
/// (CLI character render, SSE forwarder, no-op for batch callers).
///
/// All methods default to no-ops so callers override only the deltas they
/// care about. The contract: deltas for one block arrive in order; blocks may
/// be interleaved by `id`; `on_done` is the last call and carries the final
/// `StopReason` that will appear in the returned `Response`.
///
/// Concurrency. Sinks are not thread-safe; the provider drives them from
/// whichever coroutine ran `send`. Sinks that need to dispatch elsewhere are
/// responsible for hopping onto their own strand.
class EventSink {
public:
  EventSink() = default;
  virtual ~EventSink() = default;

  EventSink(const EventSink&) = delete;
  EventSink& operator=(const EventSink&) = delete;
  EventSink(EventSink&&) = delete;
  EventSink& operator=(EventSink&&) = delete;

  /// A delta of free-form assistant text.
  virtual void on_text_delta(std::string_view delta) {
    static_cast<void>(delta);
  }

  /// A delta of extended-thinking text (Anthropic) / reasoning trace.
  virtual void on_thinking_delta(std::string_view delta) {
    static_cast<void>(delta);
  }

  /// The provider opened a `tool_use` block. `id` is the vendor-issued unique
  /// id; `name` is the tool name.
  virtual void on_tool_start(std::string_view id, std::string_view name) {
    static_cast<void>(id);
    static_cast<void>(name);
  }

  /// A partial chunk of the JSON input for the tool block identified by `id`.
  virtual void on_tool_delta(std::string_view id, std::string_view input_delta) {
    static_cast<void>(id);
    static_cast<void>(input_delta);
  }

  /// Terminal callback. `stop_reason` matches the `Response::stop_reason` that
  /// `send` will return.
  virtual void on_done(core::StopReason stop_reason) {
    static_cast<void>(stop_reason);
  }
};

/// Abstract entry point for one provider call. Implementations are the
/// adapter set (Anthropic, OpenAI, …) plus `FakeProvider` for tests; the
/// runtime owns one instance per active `Route`.
///
/// The method is `const` by design: providers may be shared across concurrent
/// turns from different agents on the same process. Any internal mutable
/// state (retry counters, scripted-turn cursors) lives behind synchronisation
/// in the concrete subclass.
class System {
public:
  System() = default;
  virtual ~System() = default;

  System(const System&) = delete;
  System& operator=(const System&) = delete;
  System(System&&) = delete;
  System& operator=(System&&) = delete;

  /// Drive one turn end-to-end. Returns the assembled `Response` on success,
  /// or an `Error` carrying the protocol-classified category from
  /// `docs/design-docs/api-portability.md` "Error Categories".
  ///
  /// `sink` is optional: if non-null the provider calls into it with stream
  /// deltas before returning. If null the provider still synthesises and
  /// returns the same `Response`. The sink is borrowed for the duration of
  /// the call only.
  [[nodiscard]] virtual async::Awaitable<core::Result<Response>>
  send(Request request, Route route, EventSink* sink = nullptr) const = 0;
};

}  // namespace orangutan::provider
