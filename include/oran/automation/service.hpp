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

namespace orangutan::hook {
class Bus;
}  // namespace orangutan::hook

namespace orangutan::automation {

struct MemoryRetentionHookOptions {
  hook::Bus* bus{};
  std::string source{"periodic"};
  std::string agent_key{"automation"};
  std::string identity{"retention"};
};

struct MemoryRetentionServiceOptions {
  MemoryRetentionHookOptions hooks{};
};

struct MemoryRetentionTickRequest {
  std::string job_key;
  core::Time now{core::Time::epoch()};
};

struct MemoryRetentionHookPublishResult {
  std::size_t sink_count{0};
  std::size_t failure_count{0};
};

struct MemoryRetentionTickResult {
  std::string job_key;
  PeriodicEvaluation schedule{};
  bool ran{false};
  std::size_t shadowed_count{0};
  std::optional<MemoryRetentionJobRecord> job{};
  std::optional<MemoryRetentionRunRecord> run{};
  std::optional<MemoryRetentionHookPublishResult> hook_publish{};
};

/// One caller-driven tick for the stored long-term retention job.
///
/// This owner intentionally does not start a background loop, acquire leases, or
/// own timers. A future service loop can call `tick(...)` when it owns those
/// process-level concerns. When constructed with a hook bus, a due tick
/// publishes advisory job lifecycle metadata around backend work and publishes
/// `memory_decay` metadata after successful durable state advances.
class MemoryRetentionService {
public:
  MemoryRetentionService(AutomationRepository& repository,
                         memory::longterm::Backend& backend,
                         MemoryRetentionServiceOptions options = {}) noexcept;

  [[nodiscard]] async::Awaitable<core::Result<MemoryRetentionTickResult>> tick(MemoryRetentionTickRequest request);

private:
  AutomationRepository* repository_{};
  memory::longterm::Backend* backend_{};
  MemoryRetentionServiceOptions options_{};
};

}  // namespace orangutan::automation
