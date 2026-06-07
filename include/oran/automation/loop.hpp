// include/oran/automation/loop.hpp - caller-started automation loop steps.

#pragma once

#include <chrono>
#include <string>

#include <asio/any_io_executor.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/automation/service.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>

namespace orangutan::automation {

struct MemoryRetentionLoopRunOnceRequest {
  std::string job_key;
  core::Time now{core::Time::epoch()};
  std::chrono::steady_clock::duration max_wait{std::chrono::steady_clock::duration::zero()};
};

struct MemoryRetentionLoopRunOnceResult {
  std::chrono::nanoseconds waited_for{0};
  MemoryRetentionTickResult tick{};
};

/// Caller-started retention loop step.
///
/// This owner can wait until one stored retention job is due, then delegate to
/// `MemoryRetentionService::tick(...)`. It is intentionally a single explicit
/// awaitable, not a detached background loop.
class MemoryRetentionLoop {
public:
  MemoryRetentionLoop(asio::any_io_executor executor, MemoryRetentionService service) noexcept;

  [[nodiscard]] async::Awaitable<core::Result<MemoryRetentionLoopRunOnceResult>>
  run_once(MemoryRetentionLoopRunOnceRequest request);

private:
  asio::any_io_executor executor_;
  MemoryRetentionService service_;
};

}  // namespace orangutan::automation
