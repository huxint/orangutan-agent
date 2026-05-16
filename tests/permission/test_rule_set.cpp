// tests/permission/test_rule_set.cpp — `RuleSet` evaluator + glob matcher.

#include <optional>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <oran/permission.hpp>

using namespace std::string_view_literals;

namespace perm = orangutan::permission;
using perm::Mode;
using perm::Rule;
using perm::RuleSet;
using perm::Verdict;

TEST_CASE("glob_match handles literal and wildcard patterns", "[unit][permission][glob]") {
  REQUIRE(perm::glob_match("file.read"sv, "file.read"sv));
  REQUIRE_FALSE(perm::glob_match("file.read"sv, "file.write"sv));
  REQUIRE(perm::glob_match("file.*"sv, "file.read"sv));
  REQUIRE(perm::glob_match("file.*"sv, "file.write"sv));
  REQUIRE(perm::glob_match("file.*"sv, "file."sv));
  REQUIRE_FALSE(perm::glob_match("file.*"sv, "shell.exec"sv));
  REQUIRE(perm::glob_match("*"sv, ""sv));
  REQUIRE(perm::glob_match("*"sv, "anything"sv));
  REQUIRE(perm::glob_match("a*c"sv, "ac"sv));
  REQUIRE(perm::glob_match("a*c"sv, "abc"sv));
  REQUIRE(perm::glob_match("a*c"sv, "abbbbbc"sv));
  REQUIRE_FALSE(perm::glob_match("a*c"sv, "ab"sv));
  // Multiple wildcards: backtracking required.
  REQUIRE(perm::glob_match("*.*"sv, "tool.read"sv));
  REQUIRE_FALSE(perm::glob_match("*.*"sv, "noseparator"sv));
}

TEST_CASE("empty RuleSet falls back to mode default", "[unit][permission][rule_set]") {
  RuleSet rs;
  REQUIRE(rs.size() == 0);
  REQUIRE(rs.evaluate("file.read", Mode::strict).verdict == Verdict::deny);
  REQUIRE(rs.evaluate("file.read", Mode::sandboxed).verdict == Verdict::deny);
  REQUIRE(rs.evaluate("file.read", Mode::default_).verdict == Verdict::ask);
  REQUIRE(rs.evaluate("file.read", Mode::permissive).verdict == Verdict::allow);
}

TEST_CASE("RuleSet allow rules match by exact name and glob", "[unit][permission][rule_set]") {
  RuleSet rs;
  rs.add(Rule{.verdict = Verdict::allow, .tool_pattern = "file.read", .capability = std::nullopt});
  rs.add(Rule{.verdict = Verdict::allow, .tool_pattern = "shell.*", .capability = std::nullopt});

  REQUIRE(rs.evaluate("file.read", Mode::strict).verdict == Verdict::allow);
  REQUIRE(rs.evaluate("shell.exec", Mode::strict).verdict == Verdict::allow);
  REQUIRE(rs.evaluate("shell.ls", Mode::strict).verdict == Verdict::allow);
  // Unmatched in strict mode -> deny.
  REQUIRE(rs.evaluate("network.fetch", Mode::strict).verdict == Verdict::deny);
}

TEST_CASE("RuleSet deny wins over allow regardless of order", "[unit][permission][rule_set]") {
  RuleSet rs;
  rs.add(Rule{.verdict = Verdict::allow, .tool_pattern = "shell.*", .capability = std::nullopt});
  rs.add(Rule{.verdict = Verdict::deny, .tool_pattern = "shell.rm", .capability = std::nullopt});

  auto blocked = rs.evaluate("shell.rm", Mode::permissive);
  REQUIRE(blocked.verdict == Verdict::deny);
  REQUIRE(blocked.reason.find("deny") != std::string::npos);

  auto allowed = rs.evaluate("shell.ls", Mode::permissive);
  REQUIRE(allowed.verdict == Verdict::allow);
  REQUIRE(allowed.reason.find("allow") != std::string::npos);
}

TEST_CASE("RuleSet ask fires when no allow/deny matches", "[unit][permission][rule_set]") {
  RuleSet rs;
  rs.add(Rule{.verdict = Verdict::allow, .tool_pattern = "file.read", .capability = std::nullopt});
  rs.add(Rule{.verdict = Verdict::ask, .tool_pattern = "file.write", .capability = std::nullopt});

  auto write = rs.evaluate("file.write", Mode::strict);
  REQUIRE(write.verdict == Verdict::ask);
  REQUIRE(write.reason.find("ask") != std::string::npos);
}

TEST_CASE("RuleSet returns the index of the first matching rule in its reason", "[unit][permission][rule_set]") {
  RuleSet rs;
  rs.add(Rule{.verdict = Verdict::allow, .tool_pattern = "x", .capability = std::nullopt});        // 0
  rs.add(Rule{.verdict = Verdict::deny, .tool_pattern = "shell.rm", .capability = std::nullopt});  // 1
  rs.add(Rule{.verdict = Verdict::deny, .tool_pattern = "shell.*", .capability = std::nullopt});   // 2

  auto decision = rs.evaluate("shell.rm", Mode::permissive);
  REQUIRE(decision.verdict == Verdict::deny);
  REQUIRE(decision.reason.find("rule #1") != std::string::npos);
}

TEST_CASE("RuleSet::clear empties the set", "[unit][permission][rule_set]") {
  RuleSet rs;
  rs.add(Rule{.verdict = Verdict::allow, .tool_pattern = "x", .capability = std::nullopt});
  REQUIRE(rs.size() == 1);
  rs.clear();
  REQUIRE(rs.size() == 0);
  REQUIRE(rs.evaluate("x", Mode::strict).verdict == Verdict::deny);
}

TEST_CASE("Verdict and Mode have stable string mappings", "[unit][permission]") {
  REQUIRE(perm::to_string_view(Verdict::allow) == "allow");
  REQUIRE(perm::to_string_view(Verdict::deny) == "deny");
  REQUIRE(perm::to_string_view(Verdict::ask) == "ask");
  REQUIRE(perm::to_string_view(Mode::strict) == "strict");
  REQUIRE(perm::to_string_view(Mode::default_) == "default");
  REQUIRE(perm::to_string_view(Mode::permissive) == "permissive");
  REQUIRE(perm::to_string_view(Mode::sandboxed) == "sandboxed");
}
