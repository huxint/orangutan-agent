// src/oran-permission/materialize.cpp — three-layer rule merge.

#include <oran/permission/materialize.hpp>

#include <oran/config/config.hpp>
#include <oran/permission/defaults.hpp>
#include <oran/permission/rule_set.hpp>

namespace orangutan::permission {

namespace {

[[nodiscard]] Verdict to_verdict(config::PermissionVerdict v) noexcept {
  switch (v) {
    case config::PermissionVerdict::allow:
      return Verdict::allow;
    case config::PermissionVerdict::deny:
      return Verdict::deny;
    case config::PermissionVerdict::ask:
      return Verdict::ask;
  }
  return Verdict::deny;
}

void append_layer(RuleSet& rs, const config::PermissionsConfig& layer) {
  for (const auto& rule : layer.rules) {
    rs.add(Rule{
        .verdict = to_verdict(rule.verdict),
        .tool_pattern = rule.tool_pattern,
        .capability = rule.capability,
    });
  }
}

}  // namespace

RuleSet materialize(Mode mode, const config::PermissionsConfig& global, const config::PermissionsConfig& per_agent) {
  auto rs = Defaults::for_mode(mode);
  append_layer(rs, global);
  append_layer(rs, per_agent);
  return rs;
}

RuleSet materialize(Mode mode, const config::PermissionsConfig& global) {
  return materialize(mode, global, config::PermissionsConfig{});
}

}  // namespace orangutan::permission
