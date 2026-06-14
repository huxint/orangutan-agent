// bench/permission/scenarios/materialize.cpp
//
// A-vs-B coverage for `permission::materialize`, the three-layer
// rule merge (defaults → global config → per-agent overlay).
//
//   1. `permission.materialize_defaults_only`             :
//        `materialize(Mode::default_, empty, empty)`. Documents
//        the bare cost of running the three-layer concatenation
//        when only the built-in defaults contribute.
//   2. `permission.materialize_with_global`               :
//        Same call with a populated 8-rule global block. The
//        delta vs. (1) is the per-rule cost of appending
//        `config::PermissionRuleConfig` entries.
//   3. `permission.materialize_with_global_and_agent`     :
//        Full three-layer merge with the same global + a 2-rule
//        per-agent overlay. Documents the additional cost of
//        the third layer; the overlay path is the one
//        `oran-bootstrap`'s future per-agent runtime assembly
//        will exercise.

#include <nanobench.h>

#include <utility>

#include <oran/config.hpp>
#include <oran/core/capability.hpp>
#include <oran/permission.hpp>

namespace orangutan::bench {

namespace {

using core::Capability;
using permission::Mode;
using permission::RuleSet;
namespace cfg = orangutan::config;

[[nodiscard]] cfg::PermissionRuleConfig
rule(cfg::PermissionVerdict v, std::string pattern, std::optional<Capability> cap) {
  return cfg::PermissionRuleConfig{
      .verdict = v,
      .tool_pattern = std::move(pattern),
      .capability = cap,
  };
}

[[nodiscard]] cfg::PermissionsConfig make_global_fixture() {
  cfg::PermissionsConfig out;
  out.rules.reserve(8);
  out.rules.push_back(rule(cfg::PermissionVerdict::allow, "FileRead", std::nullopt));
  out.rules.push_back(rule(cfg::PermissionVerdict::allow, "FileSearch", std::nullopt));
  out.rules.push_back(rule(cfg::PermissionVerdict::allow, "*", Capability::read_memory));
  out.rules.push_back(rule(cfg::PermissionVerdict::deny, "*", Capability::runtime_loader));
  out.rules.push_back(rule(cfg::PermissionVerdict::deny, "ShellExec(rm:*)", std::nullopt));
  out.rules.push_back(rule(cfg::PermissionVerdict::ask, "FileWrite", std::nullopt));
  out.rules.push_back(rule(cfg::PermissionVerdict::ask, "*", Capability::spawn_subprocess));
  out.rules.push_back(rule(cfg::PermissionVerdict::ask, "*", Capability::egress_websocket));
  return out;
}

[[nodiscard]] cfg::PermissionsConfig make_agent_fixture() {
  cfg::PermissionsConfig out;
  out.rules.reserve(2);
  out.rules.push_back(rule(cfg::PermissionVerdict::allow, "*", Capability::egress_http));
  out.rules.push_back(rule(cfg::PermissionVerdict::deny, "ShellExec(git push *)", std::nullopt));
  return out;
}

[[gnu::noinline]] RuleSet materialize_defaults_only() {
  return std::move(*permission::materialize(Mode::default_, cfg::PermissionsConfig{}, cfg::PermissionsConfig{}));
}

[[gnu::noinline]] RuleSet materialize_with_global(const cfg::PermissionsConfig& global) {
  return std::move(*permission::materialize(Mode::default_, global, cfg::PermissionsConfig{}));
}

[[gnu::noinline]] RuleSet materialize_with_global_and_agent(const cfg::PermissionsConfig& global,
                                                            const cfg::PermissionsConfig& agent) {
  return std::move(*permission::materialize(Mode::default_, global, agent));
}

}  // namespace

void register_materialize_scenarios(ankerl::nanobench::Bench& bench) {
  const auto global = make_global_fixture();
  const auto agent = make_agent_fixture();

  bench.run("permission.materialize_defaults_only", [&] {
    auto rs = materialize_defaults_only();
    ankerl::nanobench::doNotOptimizeAway(rs);
  });
  bench.run("permission.materialize_with_global", [&] {
    auto rs = materialize_with_global(global);
    ankerl::nanobench::doNotOptimizeAway(rs);
  });
  bench.run("permission.materialize_with_global_and_agent", [&] {
    auto rs = materialize_with_global_and_agent(global, agent);
    ankerl::nanobench::doNotOptimizeAway(rs);
  });
}

}  // namespace orangutan::bench
