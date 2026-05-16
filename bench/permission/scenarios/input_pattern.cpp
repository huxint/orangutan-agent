// bench/permission/scenarios/input_pattern.cpp
//
// A-vs-B coverage for re2-backed `Rule::input_pattern` matching:
//
//   1. `permission.input_pattern_match`   : evaluate a rule whose
//                                            `input_pattern` accepts the
//                                            supplied call input. Pays
//                                            re2's PartialMatch on the
//                                            success path.
//   2. `permission.input_pattern_miss`    : evaluate a rule whose
//                                            `input_pattern` rejects the
//                                            supplied input. Pays re2's
//                                            PartialMatch on the failure
//                                            path and then falls through
//                                            to the mode default. The
//                                            two scenarios together
//                                            document the input-regex
//                                            cost end-to-end so a future
//                                            regression can compare match
//                                            and miss budgets.
//   3. `permission.no_input_pattern`      : the same rule set without an
//                                            `input_pattern` on the firing
//                                            rule, evaluated with the same
//                                            call. Documents the cost
//                                            *removed* by avoiding the
//                                            re2 path when no pattern is
//                                            authored.

#include <nanobench.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <oran/core/capability.hpp>
#include <oran/permission.hpp>

namespace orangutan::bench {

namespace {

using permission::InputPattern;
using permission::Mode;
using permission::Rule;
using permission::RuleSet;
using permission::Verdict;

[[nodiscard]] RuleSet rule_set_with_pattern(const std::string& pattern) {
  auto pat = InputPattern::compile(pattern);
  RuleSet rs;
  rs.add(Rule{
      .verdict = Verdict::deny,
      .tool_pattern = "shell.exec",
      .input_pattern = std::move(*pat),
  });
  return rs;
}

[[nodiscard]] RuleSet rule_set_without_pattern() {
  RuleSet rs;
  rs.add(Rule{
      .verdict = Verdict::deny,
      .tool_pattern = "shell.exec",
  });
  return rs;
}

}  // namespace

void register_input_pattern_scenarios(ankerl::nanobench::Bench& b) {
  const auto patterned_rs = rule_set_with_pattern("^rm ");
  const auto plain_rs = rule_set_without_pattern();
  const auto matching_input = std::string_view{"rm -rf /tmp/scratch"};
  const auto non_matching_input = std::string_view{"ls -la /tmp/scratch"};

  b.run("permission.input_pattern_match", [&] {
    auto decision = patterned_rs.evaluate("shell.exec", matching_input, {}, Mode::permissive);
    ankerl::nanobench::doNotOptimizeAway(decision.verdict);
  });

  b.run("permission.input_pattern_miss", [&] {
    auto decision = patterned_rs.evaluate("shell.exec", non_matching_input, {}, Mode::permissive);
    ankerl::nanobench::doNotOptimizeAway(decision.verdict);
  });

  b.run("permission.no_input_pattern", [&] {
    auto decision = plain_rs.evaluate("shell.exec", matching_input, {}, Mode::permissive);
    ankerl::nanobench::doNotOptimizeAway(decision.verdict);
  });
}

}  // namespace orangutan::bench
