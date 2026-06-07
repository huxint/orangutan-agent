// include/oran/automation/service.hpp - explicit automation service ticks.

#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/automation/repository.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>

namespace orangutan::memory::longterm {
class Backend;
}  // namespace orangutan::memory::longterm

namespace orangutan::automation {

struct MemoryRetentionTickRequest {
  std::string job_key;
  core::Time now{core::Time::epoch()};
};

struct MemoryRetentionTickResult {
  std::string job_key;
  PeriodicEvaluation schedule{};
  bool ran{false};
  std::size_t shadowed_count{0};
  std::optional<MemoryRetentionJobRecord> job{};
  std::optional<MemoryRetentionRunRecord> run{};
};

/// One caller-driven tick for the stored long-term retention job.
///
/// This owner intentionally does not start a background loop, acquire leases, or
/// publish hooks. A future service loop can call `tick(...)` when it owns those
/// process-level concerns.
class MemoryRetentionService {
public:
  MemoryRetentionService(AutomationRepository& repository, memory::longterm::Backend& backend) noexcept;

  [[nodiscard]] async::Awaitable<core::Result<MemoryRetentionTickResult>> tick(MemoryRetentionTickRequest request);

private:
  AutomationRepository* repository_{};
  memory::longterm::Backend* backend_{};
};

}  // namespace orangutan::automation
