// bench/permission/scenarios/rule_set.cpp
//
// A-vs-B coverage for the permission evaluator:
//
//   1. `permission.rule_set_evaluate`  : `RuleSet::evaluate` walks the rules
//                                        in three precedence passes
//                                        (deny → allow → ask) over a
//                                        16-rule fixture and produces a
//                                        Decision with a formatted reason.
//   2. `permission.linear_find_if`     : `std::ranges::find_if` over the
//                                        same rules using a single pass
//                                        that returns the *first* match
//                                        (no precedence respected). This
//                                        documents the cost of the
//                                        precedence-respecting walk over
//                                        the cheapest possible matcher.
//
// Both scenarios target the same 16-rule fixture and evaluate one
// representative tool name per iteration.

#include <nanobench.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <oran/permission.hpp>

namespace orangutan::bench {

namespace {

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
    });
  }
  for (int i = 0; i < 4; ++i) {
    rules.push_back(Rule{
        .verdict = Verdict::ask,
        .tool_pattern = "file.write_" + std::to_string(i),
    });
  }
  for (int i = 0; i < 4; ++i) {
    rules.push_back(Rule{
        .verdict = Verdict::deny,
        .tool_pattern = "shell.rm_" + std::to_string(i),
    });
  }
  rules.push_back(Rule{.verdict = Verdict::allow, .tool_pattern = "shell.*"});
  rules.push_back(Rule{.verdict = Verdict::deny, .tool_pattern = "shell.rm"});
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
}

}  // namespace orangutan::bench
