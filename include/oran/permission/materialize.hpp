#pragma once

#include <oran/config/config.hpp>
#include <oran/core/result.hpp>
#include <oran/permission/rule_set.hpp>

namespace orangutan::permission {

/// Append baseline, global and agent rules in that order. Explicit denies
/// survive overlays; invalid input patterns return an error.
[[nodiscard]] core::Result<RuleSet>
materialize(Mode mode, const config::PermissionsConfig& global, const config::PermissionsConfig& per_agent);

[[nodiscard]] core::Result<RuleSet> materialize(Mode mode, const config::PermissionsConfig& global);

}  // namespace orangutan::permission
