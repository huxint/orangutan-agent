// include/oran/automation/webhook.hpp - webhook-trigger producer seam.

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/automation/runtime.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>

namespace orangutan::automation {

/// Deterministic trigger key used by webhook producers and stored triggered job
/// descriptors. A configured triggered job with `trigger_key = "webhook:ci"`
/// matches `webhook_trigger_key("ci")`.
[[nodiscard]] core::Result<std::string> webhook_trigger_key(std::string_view webhook_key);

struct WebhookTriggerRequest {
  std::string webhook_key;
  std::optional<std::string> payload{};
  core::Time received_at{core::Time::epoch()};
  std::size_t job_limit{100};
};

struct WebhookTriggerResult {
  std::string trigger_key;
  TriggeredQueueEnqueueResult enqueue{};
};

/// Caller-owned producer for non-chat external triggers.
///
/// This is intentionally only the automation boundary: it validates and
/// normalizes a webhook id, preserves the optional payload for handlers, and
/// enqueues matching triggered job descriptors through the supplied
/// `AutomationService`. HTTP listener ownership stays with a future interface
/// slice.
class WebhookProducer {
public:
  explicit WebhookProducer(AutomationService& service) noexcept;

  [[nodiscard]] async::Awaitable<core::Result<WebhookTriggerResult>> trigger(WebhookTriggerRequest request);

private:
  AutomationService* service_{};
};

}  // namespace orangutan::automation
