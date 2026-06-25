// include/oran/bootstrap/automation_cron.hpp - automation config mapping helpers.

#pragma once

#include <vector>

#include <oran/automation.hpp>
#include <oran/core/result.hpp>

namespace orangutan::config {
class Config;
}  // namespace orangutan::config

namespace orangutan::bootstrap {

/// Map operator-authored cron config into automation-owned repository seeds.
///
/// `oran-config` owns typed JSON shape and UTC timestamps. Bootstrap is the
/// composition root that can validate cron expressions through `oran-automation`
/// without making the lower config library depend on scheduling code.
[[nodiscard]] core::Result<std::vector<automation::UpsertCronJobRequest>> cron_jobs_from(const config::Config& cfg);

/// Map operator-authored triggered-job config into automation-owned repository
/// seeds. The automation library owns durable descriptor validation; bootstrap
/// only copies the typed config fields across the dependency boundary.
[[nodiscard]] core::Result<std::vector<automation::UpsertTriggeredJobRequest>>
triggered_jobs_from(const config::Config& cfg);

}  // namespace orangutan::bootstrap
