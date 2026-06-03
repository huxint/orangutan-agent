// src/oran-permission/materialize.cpp — three-layer rule merge.

#include <oran/permission/materialize.hpp>

#include <chrono>
#include <expected>
#include <utility>

#include <oran/config/config.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/result.hpp>
#include <oran/permission/defaults.hpp>
#include <oran/permission/input_pattern.hpp>
#include <oran/permission/rule_set.hpp>

namespace orangutan::permission {

namespace {

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
    const auto verdict = core::parse_enum<Verdict>(core::enum_name(rule.verdict)).value_or(Verdict::deny);
    Rule runtime_rule{
        .verdict = verdict,
        .tool_pattern = rule.tool_pattern,
        .capability = rule.capability,
        .input_pattern = std::move(input_pattern),
    };
    if (rule.replay_max.has_value()) {
      runtime_rule.replay_max = *rule.replay_max;
    }
    if (rule.approval_ttl_seconds.has_value()) {
      runtime_rule.approval_ttl = std::chrono::seconds{*rule.approval_ttl_seconds};
    }
    rs.add(std::move(runtime_rule));
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
