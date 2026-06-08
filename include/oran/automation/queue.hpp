// include/oran/automation/queue.hpp - caller-owned triggered job queue.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <asio/any_io_executor.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/automation/service.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>

namespace orangutan::automation {

enum class TriggeredQueueOverflowPolicy : std::uint8_t {
  drop_newest,
};

enum class TriggeredQueueDropReason : std::uint8_t {
  queue_full,
};

struct TriggeredQueuedJob {
  TriggeredExecutionJob execution{};
  core::Time enqueued_at{core::Time::epoch()};
};

struct TriggeredDroppedJob {
  TriggeredExecutionJob execution{};
  TriggeredQueueDropReason reason{TriggeredQueueDropReason::queue_full};
  core::Time dropped_at{core::Time::epoch()};
  std::size_t queue_capacity{0};
  std::size_t queue_size{0};
};

struct TriggeredQueueOptions {
  std::size_t capacity{64};
  TriggeredQueueOverflowPolicy overflow_policy{TriggeredQueueOverflowPolicy::drop_newest};
  TriggeredHookOptions hooks{};
};

struct TriggeredQueueEnqueueRequest {
  std::string trigger_key;
  core::Time received_at{core::Time::epoch()};
  std::size_t job_limit{100};
};

struct TriggeredQueueEnqueueResult {
  TriggeredIntakeResult intake{};
  std::size_t enqueued_count{0};
  std::size_t dropped_count{0};
  std::vector<TriggeredQueuedJob> enqueued{};
  std::vector<TriggeredDroppedJob> dropped{};
};

struct TriggeredQueueDrainOnceRequest {
  TriggeredJobHandler handler{};
};

struct TriggeredQueueDrainOnceResult {
  TriggeredQueuedJob queued{};
  TriggeredExecuteOneResult execution{};
};

/// Caller-owned bounded queue for matched triggered job descriptors.
///
/// `enqueue(...)` reuses `TriggeredService::intake(...)`, pushes matched jobs
/// into bounded in-process state, and applies explicit overflow policy. It does
/// not notify channels, call agents, or start a background drain loop.
/// Consumers must explicitly `receive()` queued jobs or call `drain_once(...)`
/// to execute exactly one queued descriptor through the supplied handler.
class TriggeredQueue {
public:
  TriggeredQueue(asio::any_io_executor executor, TriggeredService service, TriggeredQueueOptions options = {});
  ~TriggeredQueue();

  TriggeredQueue(const TriggeredQueue&) = delete;
  TriggeredQueue& operator=(const TriggeredQueue&) = delete;
  TriggeredQueue(TriggeredQueue&&) noexcept;
  TriggeredQueue& operator=(TriggeredQueue&&) noexcept;

  [[nodiscard]] async::Awaitable<core::Result<TriggeredQueueEnqueueResult>>
  enqueue(TriggeredQueueEnqueueRequest request);
  [[nodiscard]] async::Awaitable<core::Result<TriggeredQueuedJob>> receive();
  [[nodiscard]] async::Awaitable<core::Result<TriggeredQueueDrainOnceResult>>
  drain_once(TriggeredQueueDrainOnceRequest request);

  void close() noexcept;
  [[nodiscard]] std::size_t capacity() const noexcept;
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] bool closed() const;
  [[nodiscard]] TriggeredService& service() noexcept;
  [[nodiscard]] const TriggeredService& service() const noexcept;

private:
  struct Impl;

  std::unique_ptr<Impl> impl_;
};

}  // namespace orangutan::automation
