// include/oran/bootstrap/memory_retention.hpp - retention config mapping helpers.

#pragma once

#include <string_view>

#include <oran/automation.hpp>
#include <oran/bootstrap/runtime_assembly.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>

namespace orangutan::config {
class Config;
}  // namespace orangutan::config

namespace orangutan::bootstrap {

/// Map the operator-facing long-term retention config into the automation-owned
/// periodic job descriptor. Bootstrap is the composition root here: automation
/// owns scheduler units, config owns parsed policy, and memory owns the eventual
/// decay request/backend.
[[nodiscard]] core::Result<automation::MemoryRetentionJob>
longterm_memory_retention_job_from(const config::Config& cfg, std::string_view scope_key, core::Time first_fire_at);

/// Derive the one-shot startup decay options from the same retention job
/// descriptor so startup retention and future periodic retention cannot drift.
[[nodiscard]] LongtermMemoryStartupDecayOptions
longterm_memory_startup_decay_options_from(const automation::MemoryRetentionJob& job, core::Time decay_at);

}  // namespace orangutan::bootstrap
