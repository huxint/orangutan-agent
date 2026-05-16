// bench/permission/scenarios/defaults.cpp
//
// A-vs-B coverage for `Defaults::for_mode`:
//
//   1. `permission.defaults_build_default`     : the factory call that
//                                                builds a `RuleSet` for
//                                                `Mode::default_`. This
//                                                is the path the future
//                                                config-loading slice
//                                                pays at startup.
//   2. `permission.defaults_hand_built_default`: the same `RuleSet`
//                                                built inline, without
//                                                going through the
//                                                factory. Documents the
//                                                cost of the factory
//                                                (function-call frame +
//                                                two `RuleSet` moves)
//                                                vs. inline construction
//                                                so future callers can
//                                                decide whether the
//                                                factory is worth the
//                                                overhead in their
//                                                hot path. At startup
//                                                cost the answer is
//                                                "definitely yes"; the
//                                                A/B is for code that
//                                                rebuilds rule sets
//                                                per request.

#include <nanobench.h>

#include <oran/core/capability.hpp>
#include <oran/permission.hpp>

namespace orangutan::bench {

namespace {

using core::Capability;
using permission::Defaults;
using permission::Mode;
using permission::Rule;
using permission::RuleSet;
using permission::Verdict;

[[gnu::noinline]] RuleSet build_via_factory() {
  return Defaults::for_mode(Mode::default_);
}

[[gnu::noinline]] RuleSet build_inline_default_baseline() {
  RuleSet rs;
  rs.add(Rule{.verdict = Verdict::deny, .tool_pattern = "*", .capability = Capability::runtime_loader});
  rs.add(Rule{.verdict = Verdict::deny, .tool_pattern = "*", .capability = Capability::delete_path});
  rs.add(Rule{.verdict = Verdict::allow, .tool_pattern = "*", .capability = Capability::read_file});
  rs.add(Rule{.verdict = Verdict::allow, .tool_pattern = "*", .capability = Capability::read_memory});
  rs.add(Rule{.verdict = Verdict::ask, .tool_pattern = "*", .capability = Capability::write_file});
  rs.add(Rule{.verdict = Verdict::ask, .tool_pattern = "*", .capability = Capability::edit_file});
  rs.add(Rule{.verdict = Verdict::ask, .tool_pattern = "*", .capability = Capability::write_memory});
  rs.add(Rule{.verdict = Verdict::ask, .tool_pattern = "*", .capability = Capability::spawn_subprocess});
  rs.add(Rule{.verdict = Verdict::ask, .tool_pattern = "*", .capability = Capability::egress_http});
  return rs;
}

}  // namespace

void register_defaults_scenarios(ankerl::nanobench::Bench& bench) {
  bench.run("permission.defaults_build_default", [&] {
    auto rs = build_via_factory();
    ankerl::nanobench::doNotOptimizeAway(rs);
  });
  bench.run("permission.defaults_hand_built_default", [&] {
    auto rs = build_inline_default_baseline();
    ankerl::nanobench::doNotOptimizeAway(rs);
  });
}

}  // namespace orangutan::bench
