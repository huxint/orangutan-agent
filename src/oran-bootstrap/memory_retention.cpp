// src/oran-bootstrap/memory_retention.cpp - config-to-automation retention mapping.

#include <oran/bootstrap/memory_retention.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <string>
#include <utility>

#include <oran/config.hpp>
#include <oran/core/error.hpp>

namespace orangutan::bootstrap {
namespace {

using ::orangutan::core::Error;
using ::orangutan::core::Result;

[[nodiscard]] Result<std::size_t> checked_memory_policy_size(std::int64_t value, std::string path) {
  if (static_cast<std::uint64_t>(value) > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return std::unexpected(Error::config("memory policy value exceeds platform size range")
                               .with("path", std::move(path))
                               .with("value", std::to_string(value)));
  }
  return static_cast<std::size_t>(value);
}

}  // namespace

core::Result<automation::MemoryRetentionJob>
longterm_memory_retention_job_from(const config::Config& cfg, std::string_view scope_key, core::Time first_fire_at) {
  using namespace std::chrono;

  const auto& retention = cfg.memory().longterm.retention;
  auto max_records =
      checked_memory_policy_size(retention.max_records_per_scope, "$.memory.longterm.retention.max_records_per_scope");
  if (!max_records) {
    return std::unexpected(std::move(max_records).error());
  }

  return automation::MemoryRetentionJob{
      .scope_key = std::string{scope_key},
      .policy =
          automation::LongtermMemoryRetentionPolicy{
              .forget_after_unused = days{retention.forget_after_unused_days},
              .importance_floor = retention.importance_floor,
              .max_records_per_scope = *max_records,
              .decay_check_interval = hours{retention.decay_check_interval_hours},
          },
      .first_fire_at = first_fire_at,
  };
}

LongtermMemoryStartupDecayOptions longterm_memory_startup_decay_options_from(const automation::MemoryRetentionJob& job,
                                                                             core::Time decay_at) {
  return LongtermMemoryStartupDecayOptions{
      .scope_key = job.scope_key,
      .unused_before = core::Time{decay_at.to_system_time_point() - job.policy.forget_after_unused},
      .importance_floor = job.policy.importance_floor,
      .limit = job.policy.max_records_per_scope,
      .decay_at = decay_at,
  };
}

}  // namespace orangutan::bootstrap
