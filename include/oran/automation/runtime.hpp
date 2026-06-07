// include/oran/automation/runtime.hpp - caller-owned automation state handle.

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include <asio/any_io_executor.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/automation/service.hpp>
#include <oran/core/result.hpp>
#include <oran/storage/migrations.hpp>

namespace orangutan::automation {

struct AutomationRuntimeOptions {
  std::string database_path;
  std::size_t reader_count{2};
  std::size_t statement_cache_capacity{8};
  int busy_timeout_ms{5000};
  bool enable_wal{true};
  bool enforce_foreign_keys{true};
  AutomationRepositoryOptions repository{};
};

/// Caller-owned automation state bundle.
///
/// Opening the runtime creates parent directories, opens `automation.db`, runs
/// the automation migrations, and keeps the pool/repository lifetime stable for
/// service owners. It intentionally does not start timers, acquire leases, or
/// launch background jobs.
class AutomationRuntime {
public:
  ~AutomationRuntime();

  AutomationRuntime(const AutomationRuntime&) = delete;
  AutomationRuntime& operator=(const AutomationRuntime&) = delete;
  AutomationRuntime(AutomationRuntime&&) noexcept;
  AutomationRuntime& operator=(AutomationRuntime&&) noexcept;

  [[nodiscard]] static async::Awaitable<core::Result<AutomationRuntime>> open(asio::any_io_executor executor,
                                                                              AutomationRuntimeOptions options);

  [[nodiscard]] std::string_view database_path() const noexcept;
  [[nodiscard]] const storage::MigrationReport& migration_report() const noexcept;
  [[nodiscard]] AutomationRepository& repository() noexcept;
  [[nodiscard]] const AutomationRepository& repository() const noexcept;

  [[nodiscard]] MemoryRetentionService memory_retention_service(memory::longterm::Backend& backend,
                                                                MemoryRetentionServiceOptions options = {}) noexcept;

private:
  struct Impl;

  explicit AutomationRuntime(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

}  // namespace orangutan::automation
