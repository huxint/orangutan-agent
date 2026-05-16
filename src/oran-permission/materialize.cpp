// src/oran-permission/materialize.cpp — three-layer rule merge.

#include <oran/permission/materialize.hpp>

#include <expected>
#include <utility>

#include <oran/config/config.hpp>
#include <oran/core/result.hpp>
#include <oran/permission/defaults.hpp>
#include <oran/permission/input_pattern.hpp>
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

[[nodiscard]] core::Result<void> append_layer(RuleSet& rs, const config::PermissionsConfig& layer) {
  for (const auto& rule : layer.rules) {
    auto input_pattern = std::optional<InputPattern>{};
    if (rule.input_pattern.has_value()) {
      // Config-side parse already validated the regex; we recompile here
      // because `InputPattern` is move-only and config retains the source
      // string. A failure on this path is theoretical (the source survived
      // an earlier compile) — when it happens, propagate rather than drop
      // the rule.
      auto compiled = InputPattern::compile(*rule.input_pattern);
      if (!compiled) {
        return std::unexpected(std::move(compiled.error()));
      }
      input_pattern = std::move(*compiled);
    }
    rs.add(Rule{
        .verdict = to_verdict(rule.verdict),
        .tool_pattern = rule.tool_pattern,
        .capability = rule.capability,
        .input_pattern = std::move(input_pattern),
    });
  }
  return {};
}

}  // namespace

core::Result<RuleSet>
materialize(Mode mode, const config::PermissionsConfig& global, const config::PermissionsConfig& per_agent) {
  auto rs = Defaults::for_mode(mode);
  if (auto r = append_layer(rs, global); !r) {
    return std::unexpected(std::move(r.error()));
  }
  if (auto r = append_layer(rs, per_agent); !r) {
    return std::unexpected(std::move(r.error()));
  }
  return rs;
}

core::Result<RuleSet> materialize(Mode mode, const config::PermissionsConfig& global) {
  return materialize(mode, global, config::PermissionsConfig{});
}

}  // namespace orangutan::permission
