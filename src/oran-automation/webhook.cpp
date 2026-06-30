// src/oran-automation/webhook.cpp - webhook-trigger producer seam.

#include <oran/automation/webhook.hpp>

#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include <oran/core/error.hpp>

namespace orangutan::automation {
namespace {

[[nodiscard]] core::Error invalid_webhook_field(std::string_view field, std::string_view reason) {
  return core::Error::invalid_argument("invalid webhook trigger request")
      .with("field", std::string{field})
      .with("reason", std::string{reason});
}

}  // namespace

core::Result<std::string> webhook_trigger_key(std::string_view webhook_key) {
  if (webhook_key.empty()) {
    return std::unexpected(invalid_webhook_field("webhook_key", "empty"));
  }
  if (webhook_key.starts_with("webhook:")) {
    return std::unexpected(invalid_webhook_field("webhook_key", "must not include webhook: prefix"));
  }
  auto trigger_key = std::string{"webhook:"};
  trigger_key += webhook_key;
  return trigger_key;
}

WebhookProducer::WebhookProducer(AutomationService& service) noexcept : service_{&service} {}

async::Awaitable<core::Result<WebhookTriggerResult>> WebhookProducer::trigger(WebhookTriggerRequest request) {
  auto trigger_key = webhook_trigger_key(request.webhook_key);
  if (!trigger_key) {
    co_return std::unexpected(std::move(trigger_key).error());
  }

  auto key = std::move(*trigger_key);
  auto enqueued = co_await service_->enqueue_triggered(TriggeredQueueEnqueueRequest{
      .trigger_key = key,
      .trigger_payload = std::move(request.payload),
      .received_at = request.received_at,
      .job_limit = request.job_limit,
  });
  if (!enqueued) {
    co_return std::unexpected(std::move(enqueued).error());
  }

  co_return WebhookTriggerResult{
      .trigger_key = std::move(key),
      .enqueue = std::move(*enqueued),
  };
}

}  // namespace orangutan::automation
