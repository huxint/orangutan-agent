// src/oran-automation/queue.cpp - caller-owned triggered job queue.

#include <oran/automation/queue.hpp>

#include <expected>
#include <string>
#include <utility>

#include <oran/async/channel.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/hook/bus.hpp>
#include <oran/hook/event.hpp>
#include <oran/hook/payload.hpp>

namespace orangutan::automation {
namespace {

[[nodiscard]] core::Error invalid_triggered_queue_field(std::string field, std::string reason) {
  return core::Error::invalid_argument("triggered queue field is invalid")
      .with("field", std::move(field))
      .with("reason", std::move(reason));
}

[[nodiscard]] core::Result<void> validate_enqueue_request(const TriggeredQueueEnqueueRequest& request) {
  if (request.trigger_key.empty()) {
    return std::unexpected(invalid_triggered_queue_field("trigger_key", "empty"));
  }
  if (request.job_limit == 0) {
    return std::unexpected(invalid_triggered_queue_field("job_limit", "zero"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_queue_options(const TriggeredQueueOptions& options) {
  if (options.capacity == 0) {
    return std::unexpected(invalid_triggered_queue_field("capacity", "zero"));
  }
  return {};
}

[[nodiscard]] hook::JobDroppedPayload make_triggered_job_dropped_payload(const TriggeredHookOptions& hooks,
                                                                         const TriggeredDroppedJob& dropped) {
  return hook::JobDroppedPayload{
      .who =
          hook::Identity{
              .scope_key = {},
              .agent_key = dropped.execution.job.agent_key,
              .identity = hooks.identity,
          },
      .source = hooks.source,
      .job_key = dropped.execution.job.job_key,
      .job_type = "triggered",
      .scope_key = {},
      .trigger_key = dropped.execution.trigger_key,
      .reason = std::string{core::enum_name(dropped.reason)},
      .scheduled_at = dropped.execution.received_at,
      .dropped_at = dropped.dropped_at,
      .queue_capacity = dropped.queue_capacity,
      .queue_size = dropped.queue_size,
  };
}

async::Awaitable<void> publish_job_dropped(const TriggeredHookOptions& hooks, const TriggeredDroppedJob& dropped) {
  if (hooks.bus == nullptr) {
    co_return;
  }
  [[maybe_unused]] auto outcome =
      co_await hooks.bus->publish_advisory(hook::Event::job_dropped,
                                           make_triggered_job_dropped_payload(hooks, dropped));
}

}  // namespace

struct TriggeredQueue::Impl {
  Impl(asio::any_io_executor runtime_executor, TriggeredService service_owner, TriggeredQueueOptions queue_options)
      : executor{std::move(runtime_executor)}, service{std::move(service_owner)}, options{std::move(queue_options)},
        channel{executor, options.capacity} {}

  asio::any_io_executor executor;
  TriggeredService service;
  TriggeredQueueOptions options;
  async::Channel<TriggeredQueuedJob> channel;
};

TriggeredQueue::TriggeredQueue(asio::any_io_executor executor, TriggeredService service, TriggeredQueueOptions options)
    : impl_{std::make_unique<Impl>(std::move(executor), std::move(service), std::move(options))} {}

TriggeredQueue::~TriggeredQueue() = default;

TriggeredQueue::TriggeredQueue(TriggeredQueue&&) noexcept = default;

TriggeredQueue& TriggeredQueue::operator=(TriggeredQueue&&) noexcept = default;

async::Awaitable<core::Result<TriggeredQueueEnqueueResult>>
TriggeredQueue::enqueue(TriggeredQueueEnqueueRequest request) {
  if (auto valid = validate_queue_options(impl_->options); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }
  if (auto valid = validate_enqueue_request(request); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }

  auto intake = co_await impl_->service.intake(TriggeredIntakeRequest{
      .trigger_key = request.trigger_key,
      .received_at = request.received_at,
      .job_limit = request.job_limit,
  });
  if (!intake) {
    co_return std::unexpected(std::move(intake).error());
  }

  TriggeredQueueEnqueueResult result{.intake = std::move(*intake)};
  for (const auto& job : result.intake.jobs) {
    auto queued = TriggeredQueuedJob{
        .execution =
            TriggeredExecutionJob{
                .job = job,
                .trigger_key = result.intake.trigger_key,
                .received_at = result.intake.received_at,
            },
        .enqueued_at = result.intake.received_at,
    };

    auto sent = impl_->channel.try_send(queued);
    if (sent) {
      ++result.enqueued_count;
      result.enqueued.push_back(std::move(queued));
      continue;
    }

    if (sent.error().kind() != core::ErrorKind::mailbox_overflowed ||
        impl_->options.overflow_policy != TriggeredQueueOverflowPolicy::drop_newest) {
      co_return std::unexpected(std::move(sent).error());
    }

    auto dropped = TriggeredDroppedJob{
        .execution = std::move(queued.execution),
        .reason = TriggeredQueueDropReason::queue_full,
        .dropped_at = result.intake.received_at,
        .queue_capacity = impl_->channel.capacity(),
        .queue_size = impl_->channel.size(),
    };
    co_await publish_job_dropped(impl_->options.hooks, dropped);
    ++result.dropped_count;
    result.dropped.push_back(std::move(dropped));
  }

  co_return result;
}

async::Awaitable<core::Result<TriggeredQueuedJob>> TriggeredQueue::receive() {
  co_return co_await impl_->channel.receive();
}

void TriggeredQueue::close() noexcept {
  impl_->channel.close();
}

std::size_t TriggeredQueue::capacity() const noexcept {
  return impl_->channel.capacity();
}

std::size_t TriggeredQueue::size() const {
  return impl_->channel.size();
}

bool TriggeredQueue::closed() const {
  return impl_->channel.closed();
}

TriggeredService& TriggeredQueue::service() noexcept {
  return impl_->service;
}

const TriggeredService& TriggeredQueue::service() const noexcept {
  return impl_->service;
}

}  // namespace orangutan::automation
