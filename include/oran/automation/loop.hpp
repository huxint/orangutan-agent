// include/oran/automation/loop.hpp - caller-started automation loop steps.

#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
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
  std::string lease_owner_key{"automation-retention-loop"};
  std::chrono::steady_clock::duration lease_ttl{std::chrono::minutes{5}};
};

struct MemoryRetentionLoopRunOnceResult {
  std::chrono::nanoseconds waited_for{0};
  MemoryRetentionTickResult tick{};
};

enum class MemoryRetentionLoopRunStopReason {
  iteration_limit,
  no_due_work,
};

struct MemoryRetentionLoopRunRequest {
  std::string job_key;
  core::Time now{core::Time::epoch()};
  std::chrono::steady_clock::duration max_total_wait{std::chrono::steady_clock::duration::zero()};
  std::size_t max_iterations{1};
  std::string lease_owner_key{"automation-retention-loop"};
  std::chrono::steady_clock::duration lease_ttl{std::chrono::minutes{5}};
};

struct MemoryRetentionLoopRunResult {
  std::size_t iterations{0};
  std::size_t due_runs{0};
  std::chrono::nanoseconds waited_for{0};
  MemoryRetentionLoopRunStopReason stop_reason{MemoryRetentionLoopRunStopReason::iteration_limit};
  std::optional<MemoryRetentionLoopRunOnceResult> last_step{};
};

/// Caller-started retention loop step.
///
/// This owner can wait until one stored retention job is due, then delegate to
/// `MemoryRetentionService::tick(...)` while holding the stored job lease. It
/// can also run a finite caller-owned loop over that step. Both entry points
/// are explicit awaitables, not detached background work.
class MemoryRetentionLoop {
public:
  MemoryRetentionLoop(asio::any_io_executor executor, MemoryRetentionService service) noexcept;

  [[nodiscard]] async::Awaitable<core::Result<MemoryRetentionLoopRunOnceResult>>
  run_once(MemoryRetentionLoopRunOnceRequest request);

  [[nodiscard]] async::Awaitable<core::Result<MemoryRetentionLoopRunResult>> run(MemoryRetentionLoopRunRequest request);

private:
  asio::any_io_executor executor_;
  MemoryRetentionService service_;
};

}  // namespace orangutan::automation
