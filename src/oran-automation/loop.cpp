// src/oran-automation/loop.cpp - caller-started automation loop steps.

#include <oran/automation/loop.hpp>

#include <chrono>
#include <expected>
#include <string>
#include <utility>

#include <oran/async/sleep.hpp>
#include <oran/core/error.hpp>

namespace orangutan::automation {
namespace {

[[nodiscard]] core::Error invalid_loop_field(std::string field, std::string reason) {
  return core::Error::invalid_argument("memory retention loop field is invalid")
      .with("field", std::move(field))
      .with("reason", std::move(reason));
}

[[nodiscard]] core::Result<void> validate_run_once_request(const MemoryRetentionLoopRunOnceRequest& request) {
  if (request.max_wait < std::chrono::steady_clock::duration::zero()) {
    return std::unexpected(invalid_loop_field("max_wait", "negative"));
  }
  return {};
}

[[nodiscard]] std::chrono::nanoseconds wait_until(core::Time now, core::Time next_fire_at) noexcept {
  if (next_fire_at <= now) {
    return std::chrono::nanoseconds{0};
  }
  return std::chrono::duration_cast<std::chrono::nanoseconds>(next_fire_at.to_system_time_point() -
                                                              now.to_system_time_point());
}

[[nodiscard]] core::Time add_wait(core::Time now, std::chrono::nanoseconds waited_for) noexcept {
  return core::Time{now.to_system_time_point() + waited_for};
}

}  // namespace

MemoryRetentionLoop::MemoryRetentionLoop(asio::any_io_executor executor, MemoryRetentionService service) noexcept
    : executor_{std::move(executor)}, service_{std::move(service)} {}

async::Awaitable<core::Result<MemoryRetentionLoopRunOnceResult>>
MemoryRetentionLoop::run_once(MemoryRetentionLoopRunOnceRequest request) {
  if (auto valid = validate_run_once_request(request); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }

  auto first = co_await service_.tick(MemoryRetentionTickRequest{
      .job_key = request.job_key,
      .now = request.now,
  });
  if (!first) {
    co_return std::unexpected(std::move(first).error());
  }
  if (first->ran) {
    co_return MemoryRetentionLoopRunOnceResult{.tick = std::move(*first)};
  }

  const auto wait_for = wait_until(request.now, first->schedule.next_fire_at);
  if (wait_for > request.max_wait) {
    co_return MemoryRetentionLoopRunOnceResult{
        .waited_for = std::chrono::nanoseconds{0},
        .tick = std::move(*first),
    };
  }

  auto slept = co_await async::sleep_for(executor_, wait_for);
  if (!slept) {
    co_return std::unexpected(std::move(slept).error());
  }

  auto second = co_await service_.tick(MemoryRetentionTickRequest{
      .job_key = request.job_key,
      .now = add_wait(request.now, wait_for),
  });
  if (!second) {
    co_return std::unexpected(std::move(second).error());
  }

  co_return MemoryRetentionLoopRunOnceResult{
      .waited_for = wait_for,
      .tick = std::move(*second),
  };
}

}  // namespace orangutan::automation
