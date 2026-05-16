// tests/permission/test_materialize.cpp — `permission::materialize`
// three-layer rule merge coverage.

#include <array>
#include <span>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <oran/config.hpp>
#include <oran/core/capability.hpp>
#include <oran/permission.hpp>

namespace perm = orangutan::permission;
namespace cfg = orangutan::config;
using orangutan::core::Capability;
using perm::Defaults;
using perm::Mode;
using perm::RuleSet;
using perm::Verdict;

namespace {

[[nodiscard]] cfg::PermissionRuleConfig allow(std::string pattern, std::optional<Capability> cap = std::nullopt) {
  return cfg::PermissionRuleConfig{
      .verdict = cfg::PermissionVerdict::allow,
      .tool_pattern = std::move(pattern),
      .capability = cap,
  };
}

[[nodiscard]] cfg::PermissionRuleConfig deny(std::string pattern, std::optional<Capability> cap = std::nullopt) {
  return cfg::PermissionRuleConfig{
      .verdict = cfg::PermissionVerdict::deny,
      .tool_pattern = std::move(pattern),
      .capability = cap,
  };
}

[[nodiscard]] cfg::PermissionRuleConfig ask(std::string pattern, std::optional<Capability> cap = std::nullopt) {
  return cfg::PermissionRuleConfig{
      .verdict = cfg::PermissionVerdict::ask,
      .tool_pattern = std::move(pattern),
      .capability = cap,
  };
}

[[nodiscard]] perm::Verdict
eval(const RuleSet& rs, std::string_view tool, std::span<const Capability> caps, Mode mode = Mode::default_) {
  return rs.evaluate(tool, caps, mode).verdict;
}

[[nodiscard]] perm::Verdict eval_no_caps(const RuleSet& rs, std::string_view tool, Mode mode = Mode::default_) {
  return rs.evaluate(tool, mode).verdict;
}

}  // namespace

TEST_CASE("materialize(empty, empty) equals Defaults::for_mode", "[unit][permission][materialize]") {
  const auto rs = perm::materialize(Mode::default_, cfg::PermissionsConfig{}, cfg::PermissionsConfig{});
  REQUIRE(rs.size() == Defaults::for_mode(Mode::default_).size());

  // Spot-check representative classifications.
  const std::array<Capability, 1> read{Capability::read_file};
  const std::array<Capability, 1> dangerous{Capability::runtime_loader};
  const std::array<Capability, 1> write{Capability::write_file};
  REQUIRE(eval(rs, "any.tool", std::span<const Capability>{read}) == Verdict::allow);
  REQUIRE(eval(rs, "any.tool", std::span<const Capability>{dangerous}) == Verdict::deny);
  REQUIRE(eval(rs, "any.tool", std::span<const Capability>{write}) == Verdict::ask);
}

TEST_CASE("materialize appends global config rules after defaults", "[unit][permission][materialize]") {
  cfg::PermissionsConfig global;
  global.rules.push_back(allow("custom.tool"));

  const auto rs = perm::materialize(Mode::default_, global);
  REQUIRE(rs.size() == Defaults::for_mode(Mode::default_).size() + 1);

  // The custom.tool literal is now allowed even without a capability scope.
  REQUIRE(eval_no_caps(rs, "custom.tool") == Verdict::allow);

  // The defaults are still in place.
  const std::array<Capability, 1> read{Capability::read_file};
  REQUIRE(eval(rs, "anything", std::span<const Capability>{read}) == Verdict::allow);
}

TEST_CASE("materialize appends per-agent overlay after global", "[unit][permission][materialize]") {
  cfg::PermissionsConfig global;
  global.rules.push_back(allow("global.only"));

  cfg::PermissionsConfig overlay;
  overlay.rules.push_back(allow("agent.only"));

  const auto rs = perm::materialize(Mode::default_, global, overlay);
  REQUIRE(rs.size() == Defaults::for_mode(Mode::default_).size() + 2);
  REQUIRE(eval_no_caps(rs, "global.only") == Verdict::allow);
  REQUIRE(eval_no_caps(rs, "agent.only") == Verdict::allow);
}

TEST_CASE("materialize maps config verdicts one-to-one", "[unit][permission][materialize]") {
  // Sandboxed mode's default is deny — so a non-matching call lands on deny,
  // letting us inspect every verdict mapping without baseline interference.
  cfg::PermissionsConfig global;
  global.rules.push_back(allow("allow.it"));
  global.rules.push_back(deny("deny.it"));
  global.rules.push_back(ask("ask.it"));

  const auto rs = perm::materialize(Mode::sandboxed, global);
  REQUIRE(eval_no_caps(rs, "allow.it", Mode::sandboxed) == Verdict::allow);
  REQUIRE(eval_no_caps(rs, "deny.it", Mode::sandboxed) == Verdict::deny);
  REQUIRE(eval_no_caps(rs, "ask.it", Mode::sandboxed) == Verdict::ask);
}

TEST_CASE("materialize preserves capability scope on config-side rules", "[unit][permission][materialize]") {
  // Strict mode defaults to deny on no-match; ship an empty Defaults baseline
  // so only the config rule fires.
  cfg::PermissionsConfig global;
  global.rules.push_back(allow("*", Capability::egress_websocket));

  const auto rs = perm::materialize(Mode::strict, global);
  const std::array<Capability, 1> net{Capability::egress_websocket};
  const std::array<Capability, 1> file{Capability::read_file};

  REQUIRE(eval(rs, "any.tool", std::span<const Capability>{net}, Mode::strict) == Verdict::allow);
  // Without the required capability the rule misses and the strict-mode
  // default verdict (deny) takes over.
  REQUIRE(eval(rs, "any.tool", std::span<const Capability>{file}, Mode::strict) == Verdict::deny);
}

TEST_CASE("explicit deny in any layer outranks allow in any other layer", "[unit][permission][materialize]") {
  // Defaults::for_mode(permissive) already denies runtime_loader; ship a
  // global allow for the same capability and confirm the deny wins.
  cfg::PermissionsConfig global;
  global.rules.push_back(allow("*", Capability::runtime_loader));

  const auto rs = perm::materialize(Mode::permissive, global);
  const std::array<Capability, 1> loader{Capability::runtime_loader};
  REQUIRE(eval(rs, "any.tool", std::span<const Capability>{loader}, Mode::permissive) == Verdict::deny);

  // Mirror the same shape from the per-agent side.
  cfg::PermissionsConfig agent_only_allow;
  agent_only_allow.rules.push_back(allow("*", Capability::runtime_loader));

  const auto rs2 = perm::materialize(Mode::permissive, cfg::PermissionsConfig{}, agent_only_allow);
  REQUIRE(eval(rs2, "any.tool", std::span<const Capability>{loader}, Mode::permissive) == Verdict::deny);
}

TEST_CASE("materialize preserves intra-layer rule order", "[unit][permission][materialize]") {
  cfg::PermissionsConfig global;
  global.rules.push_back(allow("first"));
  global.rules.push_back(allow("second"));

  const auto rs = perm::materialize(Mode::strict, global);

  // Reason text encodes the rule index; the first matching rule wins at the
  // same verdict, so the index distinguishes order.
  const auto first = rs.evaluate("first", Mode::strict);
  const auto second = rs.evaluate("second", Mode::strict);
  REQUIRE(first.verdict == Verdict::allow);
  REQUIRE(second.verdict == Verdict::allow);
  REQUIRE(first.reason.contains("first"));
  REQUIRE(second.reason.contains("second"));
}

TEST_CASE("materialize two-argument overload uses an empty per-agent overlay", "[unit][permission][materialize]") {
  cfg::PermissionsConfig global;
  global.rules.push_back(allow("only.thing"));

  const auto rs = perm::materialize(Mode::strict, global);
  REQUIRE(rs.size() == 1);
  REQUIRE(eval_no_caps(rs, "only.thing", Mode::strict) == Verdict::allow);
}
