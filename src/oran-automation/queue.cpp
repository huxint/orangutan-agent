// src/oran-automation/queue.cpp - caller-owned triggered job queue.

#include <oran/automation/queue.hpp>

#include <chrono>
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

[[nodiscard]] core::Result<void> validate_drain_once_request(const TriggeredQueueDrainOnceRequest& request) {
  if (!request.handler) {
    return std::unexpected(invalid_triggered_queue_field("handler", "empty"));
  }
  if (!request.lease_owner_key.empty() && request.lease_ttl <= std::chrono::steady_clock::duration::zero()) {
    return std::unexpected(invalid_triggered_queue_field("lease_ttl", "not_positive"));
  }
  if (core::enum_name(request.blocked_agent_policy) == "unknown") {
    return std::unexpected(invalid_triggered_queue_field("blocked_agent_policy", "unknown"));
  }
  if (request.blocked_agent_policy == TriggeredQueueBlockedAgentPolicy::requeue_on_conflict) {
    return std::unexpected(invalid_triggered_queue_field("blocked_agent_policy", "unsupported_for_triggered_queue"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_drain_available_request(const TriggeredQueueDrainAvailableRequest& request) {
  if (!request.handler) {
    return std::unexpected(invalid_triggered_queue_field("handler", "empty"));
  }
  if (request.max_jobs == 0) {
    return std::unexpected(invalid_triggered_queue_field("max_jobs", "zero"));
  }
  if (!request.lease_owner_key.empty() && request.lease_ttl <= std::chrono::steady_clock::duration::zero()) {
    return std::unexpected(invalid_triggered_queue_field("lease_ttl", "not_positive"));
  }
  if (core::enum_name(request.blocked_agent_policy) == "unknown") {
    return std::unexpected(invalid_triggered_queue_field("blocked_agent_policy", "unknown"));
  }
  if (request.blocked_agent_policy == TriggeredQueueBlockedAgentPolicy::requeue_on_conflict) {
    return std::unexpected(invalid_triggered_queue_field("blocked_agent_policy", "unsupported_for_triggered_queue"));
  }
  return {};
}

[[nodiscard]] bool is_triggered_agent_lease_conflict(const core::Error& error) {
  if (error.kind() != core::ErrorKind::conflict || error.message() != "triggered agent lease is already held") {
    return false;
  }

  auto has_agent_key = false;
  auto has_owner_key = false;
  for (const auto& [key, value] : error.context()) {
    if (key == "agent_key" && !value.empty()) {
      has_agent_key = true;
    }
    if (key == "owner_key" && !value.empty()) {
      has_owner_key = true;
    }
  }
  return has_agent_key && has_owner_key;
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

namespace {

[[nodiscard]] TriggeredQueueDrainOnceRequest drain_once_request_from(TriggeredQueueDrainAvailableRequest request) {
  return TriggeredQueueDrainOnceRequest{
      .handler = std::move(request.handler),
      .lease_owner_key = std::move(request.lease_owner_key),
      .lease_ttl = request.lease_ttl,
      .blocked_agent_policy = request.blocked_agent_policy,
  };
}

}  // namespace

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
      .trigger_payload = std::move(request.trigger_payload),
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
                .trigger_payload = result.intake.trigger_payload,
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

core::Result<std::optional<TriggeredQueuedJob>> TriggeredQueue::try_receive() {
  return impl_->channel.try_receive();
}

async::Awaitable<core::Result<TriggeredQueueDrainOnceResult>>
TriggeredQueue::execute_queued(TriggeredQueuedJob queued, TriggeredQueueDrainOnceRequest request) {
  auto execution = co_await impl_->service.execute_one(TriggeredExecuteOneRequest{
      .execution = queued.execution,
      .handler = std::move(request.handler),
      .lease_owner_key = std::move(request.lease_owner_key),
      .lease_ttl = request.lease_ttl,
  });
  if (!execution) {
    if (is_triggered_agent_lease_conflict(execution.error()) &&
        request.blocked_agent_policy == TriggeredQueueBlockedAgentPolicy::drop_on_conflict) {
      auto dropped = co_await drop_queued(queued,
                                          TriggeredQueueDropReason::agent_lease_conflict,
                                          queued.execution.received_at,
                                          impl_->channel.size());
      co_return TriggeredQueueDrainOnceResult{
          .queued = std::move(queued),
          .execution = {},
          .dropped = std::move(dropped),
      };
    }
    co_return std::unexpected(std::move(execution).error());
  }

  co_return TriggeredQueueDrainOnceResult{
      .queued = std::move(queued),
      .execution = std::move(*execution),
  };
}

async::Awaitable<TriggeredDroppedJob> TriggeredQueue::drop_queued(const TriggeredQueuedJob& queued,
                                                                  TriggeredQueueDropReason reason,
                                                                  core::Time dropped_at,
                                                                  std::size_t queue_size) {
  auto dropped = TriggeredDroppedJob{
      .execution = queued.execution,
      .reason = reason,
      .dropped_at = dropped_at,
      .queue_capacity = impl_->channel.capacity(),
      .queue_size = queue_size,
  };
  co_await publish_job_dropped(impl_->options.hooks, dropped);
  co_return dropped;
}

async::Awaitable<core::Result<TriggeredQueueDrainOnceResult>>
TriggeredQueue::drain_once(TriggeredQueueDrainOnceRequest request) {
  if (auto valid = validate_drain_once_request(request); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }

  auto queued = co_await receive();
  if (!queued) {
    co_return std::unexpected(std::move(queued).error());
  }

  co_return co_await execute_queued(std::move(*queued), std::move(request));
}

async::Awaitable<core::Result<TriggeredQueueDrainAvailableResult>>
TriggeredQueue::drain_available(TriggeredQueueDrainAvailableRequest request) {
  if (auto valid = validate_drain_available_request(request); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }

  const auto max_jobs = request.max_jobs;
  auto drain_request = drain_once_request_from(std::move(request));
  TriggeredQueueDrainAvailableResult result{};
  while (result.drained_count < max_jobs) {
    auto queued = try_receive();
    if (!queued) {
      if (queued.error().kind() == core::ErrorKind::cancelled) {
        result.stop_reason = TriggeredQueueDrainAvailableStopReason::queue_closed;
        co_return result;
      }
      co_return std::unexpected(std::move(queued).error());
    }
    if (!queued->has_value()) {
      result.stop_reason = TriggeredQueueDrainAvailableStopReason::queue_empty;
      co_return result;
    }

    auto& handler = drain_request.handler;
    auto drained = co_await execute_queued(std::move(**queued),
                                           TriggeredQueueDrainOnceRequest{
                                               .handler = [&handler](TriggeredExecutionJob execution)
                                                   -> async::Awaitable<core::Result<AutomationJobHandlerResult>> {
                                                 co_return co_await handler(std::move(execution));
                                               },
                                               .lease_owner_key = drain_request.lease_owner_key,
                                               .lease_ttl = drain_request.lease_ttl,
                                               .blocked_agent_policy = drain_request.blocked_agent_policy,
                                           });
    if (!drained) {
      co_return std::unexpected(std::move(drained).error());
    }

    ++result.drained_count;
    if (drained->execution.completed) {
      ++result.completed_count;
    }
    if (!drained->execution.completed && !drained->dropped.has_value()) {
      ++result.failed_count;
    }
    if (drained->dropped.has_value()) {
      ++result.dropped_count;
    }
    result.drains.push_back(std::move(*drained));
  }

  result.stop_reason = TriggeredQueueDrainAvailableStopReason::max_jobs;
  co_return result;
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
