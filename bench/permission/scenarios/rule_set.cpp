// bench/permission/scenarios/rule_set.cpp
//
// A-vs-B coverage for the permission evaluator:
//
//   1. `permission.rule_set_evaluate`         : `RuleSet::evaluate(tool, mode)`
//                                               walks the rules in three
//                                               precedence passes
//                                               (deny → allow → ask) over a
//                                               16-rule fixture and produces
//                                               a Decision with a formatted
//                                               reason.
//   2. `permission.linear_find_if`            : `std::ranges::find_if` over the
//                                               same rules using a single pass
//                                               that returns the *first* match
//                                               (no precedence respected).
//                                               Documents the cost of the
//                                               precedence-respecting walk
//                                               over the cheapest possible
//                                               matcher.
//   3. `permission.rule_set_capability_match` : capability-aware overload
//                                               where the firing rule is
//                                               scoped to a capability the
//                                               call requires; documents the
//                                               cost of the extra optional
//                                               check on the success path.
//   4. `permission.rule_set_capability_miss`  : capability-aware overload
//                                               where every capability-bound
//                                               rule's scope excludes the
//                                               call, so the walk falls
//                                               through to the mode default;
//                                               documents the cost on the
//                                               miss side (no allocation for
//                                               a `rule #N` reason; the
//                                               fallback `default by mode=`
//                                               reason is shorter).

#include <nanobench.h>

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <oran/core/capability.hpp>
#include <oran/permission.hpp>

namespace orangutan::bench {

namespace {

using core::Capability;
using permission::glob_match;
using permission::Rule;
using permission::RuleSet;
using permission::Verdict;

[[nodiscard]] std::vector<Rule> make_fixture() {
  std::vector<Rule> rules;
  rules.reserve(16);
  for (int i = 0; i < 6; ++i) {
    rules.push_back(Rule{
        .verdict = Verdict::allow,
        .tool_pattern = "file.read_" + std::to_string(i),
        .capability = std::nullopt,
    });
  }
  for (int i = 0; i < 4; ++i) {
    rules.push_back(Rule{
        .verdict = Verdict::ask,
        .tool_pattern = "file.write_" + std::to_string(i),
        .capability = std::nullopt,
    });
  }
  for (int i = 0; i < 4; ++i) {
    rules.push_back(Rule{
        .verdict = Verdict::deny,
        .tool_pattern = "shell.rm_" + std::to_string(i),
        .capability = std::nullopt,
    });
  }
  rules.push_back(Rule{.verdict = Verdict::allow, .tool_pattern = "shell.*", .capability = std::nullopt});
  rules.push_back(Rule{.verdict = Verdict::deny, .tool_pattern = "shell.rm", .capability = std::nullopt});
  return rules;
}

[[gnu::noinline]] permission::Verdict linear_first_match(const std::vector<Rule>& rules,
                                                         std::string_view tool_name) noexcept {
  const auto it =
      std::ranges::find_if(rules, [&](const Rule& rule) noexcept { return glob_match(rule.tool_pattern, tool_name); });
  if (it == rules.end()) {
    return permission::Verdict::deny;
  }
  return it->verdict;
}

// A 16-rule fixture with capability scopes. Half the rules are capability-
// bound (mirroring the design-doc capability-aware-gating example); the
// other half are unscoped, so the precedence walk still does realistic work
// across both shapes.
[[nodiscard]] std::vector<Rule> make_capability_fixture() {
  std::vector<Rule> rules;
  rules.reserve(16);
  rules.push_back(Rule{
      .verdict = Verdict::allow,
      .tool_pattern = "file.*",
      .capability = Capability::read_file,
  });
  rules.push_back(Rule{
      .verdict = Verdict::allow,
      .tool_pattern = "file.*",
      .capability = Capability::write_file,
  });
  rules.push_back(Rule{
      .verdict = Verdict::allow,
      .tool_pattern = "shell.*",
      .capability = Capability::spawn_subprocess,
  });
  rules.push_back(Rule{
      .verdict = Verdict::allow,
      .tool_pattern = "net.*",
      .capability = Capability::egress_http,
  });
  rules.push_back(Rule{
      .verdict = Verdict::allow,
      .tool_pattern = "memory.*",
      .capability = Capability::read_memory,
  });
  rules.push_back(Rule{
      .verdict = Verdict::ask,
      .tool_pattern = "memory.*",
      .capability = Capability::write_memory,
  });
  rules.push_back(Rule{
      .verdict = Verdict::ask,
      .tool_pattern = "automation.*",
      .capability = Capability::schedule_job,
  });
  rules.push_back(Rule{
      .verdict = Verdict::deny,
      .tool_pattern = "*",
      .capability = Capability::runtime_loader,
  });
  rules.push_back(Rule{.verdict = Verdict::allow, .tool_pattern = "ping", .capability = std::nullopt});
  rules.push_back(Rule{.verdict = Verdict::allow, .tool_pattern = "version", .capability = std::nullopt});
  rules.push_back(Rule{.verdict = Verdict::allow, .tool_pattern = "clock.now", .capability = std::nullopt});
  rules.push_back(Rule{.verdict = Verdict::ask, .tool_pattern = "skill.invoke", .capability = std::nullopt});
  rules.push_back(Rule{.verdict = Verdict::ask, .tool_pattern = "agent.spawn", .capability = std::nullopt});
  rules.push_back(Rule{.verdict = Verdict::deny, .tool_pattern = "shell.rm", .capability = std::nullopt});
  rules.push_back(Rule{.verdict = Verdict::deny, .tool_pattern = "shell.fork_bomb", .capability = std::nullopt});
  rules.push_back(Rule{.verdict = Verdict::deny, .tool_pattern = "process.*", .capability = std::nullopt});
  return rules;
}

}  // namespace

void register_rule_set_scenarios(ankerl::nanobench::Bench& bench) {
  static const std::vector<Rule> fixture = make_fixture();
  static const RuleSet rule_set = [] {
    RuleSet rs;
    for (const auto& rule : fixture) {
      rs.add(rule);
    }
    return rs;
  }();
  // Pick a tool name that needs all three precedence passes (deny matches
  // late in the list, allow matches in the wildcard rule, ask never fires).
  static const std::string tool_name = "shell.rm";

  bench.run("permission.rule_set_evaluate", [&] {
    auto decision = rule_set.evaluate(tool_name, permission::Mode::permissive);
    ankerl::nanobench::doNotOptimizeAway(decision);
  });
  bench.run("permission.linear_find_if", [&] {
    auto verdict = linear_first_match(fixture, tool_name);
    ankerl::nanobench::doNotOptimizeAway(verdict);
  });

  // Slice 2: capability-aware path. Same fixture size so the numbers are
  // directly comparable against the precedence-walk baseline; the rule set
  // has capability scopes the call's required-capability span either
  // satisfies (match path) or not (miss path).
  static const RuleSet cap_rule_set = [] {
    RuleSet rs;
    for (const auto& rule : make_capability_fixture()) {
      rs.add(rule);
    }
    return rs;
  }();

  static const std::array<Capability, 2> required_for_match{
      Capability::spawn_subprocess,
      Capability::read_file,
  };
  static const std::array<Capability, 2> required_for_miss{
      // Neither value appears on any capability-bound rule in the fixture, so
      // every capability-bound rule filters out and the call falls through to
      // the unscoped rules (none of which match this tool name) and finally
      // to the mode default.
      Capability::invoke_skill,
      Capability::external_mcp,
  };
  static const std::string capability_tool = "shell.exec";

  bench.run("permission.rule_set_capability_match", [&] {
    auto decision = cap_rule_set.evaluate(capability_tool,
                                          std::span<const Capability>{required_for_match},
                                          permission::Mode::strict);
    ankerl::nanobench::doNotOptimizeAway(decision);
  });
  bench.run("permission.rule_set_capability_miss", [&] {
    auto decision = cap_rule_set.evaluate(capability_tool,
                                          std::span<const Capability>{required_for_miss},
                                          permission::Mode::strict);
    ankerl::nanobench::doNotOptimizeAway(decision);
  });
}

}  // namespace orangutan::bench
