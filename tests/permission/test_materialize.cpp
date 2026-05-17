// tests/permission/test_materialize.cpp — `permission::materialize`
// three-layer rule merge coverage.

#include <array>
#include <chrono>
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

[[nodiscard]] RuleSet
require_materialized(Mode mode, const cfg::PermissionsConfig& global, const cfg::PermissionsConfig& per_agent = {}) {
  auto rs = perm::materialize(mode, global, per_agent);
  REQUIRE(rs.has_value());
  return std::move(*rs);
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
  const auto rs = require_materialized(Mode::default_, cfg::PermissionsConfig{}, cfg::PermissionsConfig{});
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

  const auto rs = require_materialized(Mode::default_, global);
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

  const auto rs = require_materialized(Mode::default_, global, overlay);
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

  const auto rs = require_materialized(Mode::sandboxed, global);
  REQUIRE(eval_no_caps(rs, "allow.it", Mode::sandboxed) == Verdict::allow);
  REQUIRE(eval_no_caps(rs, "deny.it", Mode::sandboxed) == Verdict::deny);
  REQUIRE(eval_no_caps(rs, "ask.it", Mode::sandboxed) == Verdict::ask);
}

TEST_CASE("materialize preserves capability scope on config-side rules", "[unit][permission][materialize]") {
  // Strict mode defaults to deny on no-match; ship an empty Defaults baseline
  // so only the config rule fires.
  cfg::PermissionsConfig global;
  global.rules.push_back(allow("*", Capability::egress_websocket));

  const auto rs = require_materialized(Mode::strict, global);
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

  const auto rs = require_materialized(Mode::permissive, global);
  const std::array<Capability, 1> loader{Capability::runtime_loader};
  REQUIRE(eval(rs, "any.tool", std::span<const Capability>{loader}, Mode::permissive) == Verdict::deny);

  // Mirror the same shape from the per-agent side.
  cfg::PermissionsConfig agent_only_allow;
  agent_only_allow.rules.push_back(allow("*", Capability::runtime_loader));

  const auto rs2 = require_materialized(Mode::permissive, cfg::PermissionsConfig{}, agent_only_allow);
  REQUIRE(eval(rs2, "any.tool", std::span<const Capability>{loader}, Mode::permissive) == Verdict::deny);
}

TEST_CASE("materialize preserves intra-layer rule order", "[unit][permission][materialize]") {
  cfg::PermissionsConfig global;
  global.rules.push_back(allow("first"));
  global.rules.push_back(allow("second"));

  const auto rs = require_materialized(Mode::strict, global);

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

  auto rs_result = perm::materialize(Mode::strict, global);
  REQUIRE(rs_result.has_value());
  REQUIRE(rs_result->size() == 1);
  REQUIRE(eval_no_caps(*rs_result, "only.thing", Mode::strict) == Verdict::allow);
}

TEST_CASE("materialize compiles config-side input_pattern into the runtime Rule",
          "[unit][permission][materialize][input_pattern]") {
  cfg::PermissionsConfig global;
  global.rules.push_back(cfg::PermissionRuleConfig{
      .verdict = cfg::PermissionVerdict::deny,
      .tool_pattern = "shell.exec",
      .input_pattern = std::string{"^rm "},
  });

  const auto rs = require_materialized(Mode::permissive, global);
  // Input matches the regex -> rule fires.
  const auto blocked = rs.evaluate("shell.exec", "rm -rf /tmp", {}, Mode::permissive);
  REQUIRE(blocked.verdict == Verdict::deny);
  REQUIRE(blocked.reason.contains("input=~"));
  REQUIRE(blocked.reason.contains("^rm "));

  // Non-matching input -> falls through.
  const auto allowed = rs.evaluate("shell.exec", "ls -la", {}, Mode::permissive);
  REQUIRE(allowed.verdict == Verdict::allow);
}

TEST_CASE("materialize surfaces re2 compile failures on input_pattern",
          "[unit][permission][materialize][input_pattern]") {
  // Bypass the config-side validator by feeding an invalid pattern directly
  // into `PermissionRuleConfig`. The materialize() compile attempt should
  // produce `ErrorKind::invalid_argument`.
  cfg::PermissionsConfig global;
  global.rules.push_back(cfg::PermissionRuleConfig{
      .verdict = cfg::PermissionVerdict::deny,
      .tool_pattern = "shell.exec",
      .input_pattern = std::string{"[unclosed"},
  });

  auto rs = perm::materialize(Mode::permissive, global);
  REQUIRE_FALSE(rs.has_value());
  REQUIRE(rs.error().kind() == orangutan::core::ErrorKind::invalid_argument);
}

TEST_CASE("materialize forwards replay_max + approval_ttl_seconds from config into the Rule",
          "[unit][permission][materialize][approval_policy]") {
  cfg::PermissionsConfig global;
  global.rules.push_back(cfg::PermissionRuleConfig{
      .verdict = cfg::PermissionVerdict::ask,
      .tool_pattern = "file.write",
      .replay_max = std::uint32_t{4},
      .approval_ttl_seconds = std::int64_t{120},
  });

  const auto rs = require_materialized(Mode::strict, global);
  const auto decision = rs.evaluate("file.write", Mode::strict);
  REQUIRE(decision.verdict == Verdict::ask);
  REQUIRE(decision.replay_max == 4);
  REQUIRE(decision.approval_ttl == std::chrono::seconds{120});
}

TEST_CASE("materialize keeps Rule defaults when config omits replay_max / approval_ttl_seconds",
          "[unit][permission][materialize][approval_policy]") {
  cfg::PermissionsConfig global;
  // Both optional fields unset — operator omitted them.
  global.rules.push_back(cfg::PermissionRuleConfig{
      .verdict = cfg::PermissionVerdict::ask,
      .tool_pattern = "shell.exec",
  });

  const auto rs = require_materialized(Mode::strict, global);
  const auto decision = rs.evaluate("shell.exec", Mode::strict);
  REQUIRE(decision.verdict == Verdict::ask);
  REQUIRE(decision.replay_max == 8);
  REQUIRE(decision.approval_ttl == std::chrono::seconds{3600});
}
