// include/oran/channel/dispatch.hpp — channel→agent prompt dispatch seam.

#pragma once

#include <functional>
#include <string>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>

#include <oran/channel/channel.hpp>

namespace orangutan::channel {

class ChannelManager;

/// Normalized prompt-shaped view of one inbound channel message. The agent
/// side consumes this instead of `InboundMessage` so adapter envelopes never
/// leak into prompt-runner implementations.
struct ChannelPromptRunRequest {
  std::string channel_id;
  std::string conversation_id;
  std::string user_id;
  std::string display_name;
  std::string prompt;
  Origin origin{};
  Capabilities caps{};
  core::Time received_at{};

  friend bool operator==(const ChannelPromptRunRequest&, const ChannelPromptRunRequest&) = default;
};

struct ChannelPromptRunResult {
  std::string text;
};

/// Borrowed by `dispatch_one`; implementations run the configured agent path.
/// `oran-bootstrap` owns the concrete `AgentPromptRunner`-backed factory so
/// this library stays independent of agent/bootstrap internals.
using ChannelPromptRunner =
    std::function<async::Awaitable<core::Result<ChannelPromptRunResult>>(ChannelPromptRunRequest)>;

/// Flattens the inbound text blocks into one newline-joined prompt. Non-text
/// blocks are skipped; a message with no non-empty text is rejected with
/// `invalid_argument`.
[[nodiscard]] core::Result<ChannelPromptRunRequest> make_prompt_run_request(const InboundMessage& message);

/// Builds the outbound reply mirrored onto the inbound conversation, carrying
/// the first inbound reply reference when one exists.
[[nodiscard]] OutboundMessage make_reply_message(const InboundMessage& message, std::string text);

/// Receives one normalized message from the manager's fan-in queue, runs the
/// prompt runner, and sends the reply back through the owning adapter.
/// Caller-owned single step: no loop, no spawn; loop/cancellation policy
/// belongs to the eventual runtime-service owner.
[[nodiscard]] async::Awaitable<core::Result<DeliveryReceipt>> dispatch_one(ChannelManager& manager,
                                                                           const ChannelPromptRunner& runner);

}  // namespace orangutan::channel
