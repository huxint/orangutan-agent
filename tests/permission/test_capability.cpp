// tests/permission/test_capability.cpp — capability-aware `RuleSet::evaluate`.
//
// Covers the slice-2 addition: rules may scope to a `core::Capability` and
// only match when the call's `required_capabilities` span contains it.
// Existing scope-less behavior is unchanged and verified here against the
// new overload too.

#include <array>
#include <optional>
#include <span>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/capability.hpp>
#include <oran/permission.hpp>

namespace perm = orangutan::permission;
using orangutan::core::Capability;
using perm::Mode;
using perm::Rule;
using perm::RuleSet;
using perm::Verdict;

TEST_CASE("capability-bound rule fires when call requires the capability", "[unit][permission][capability]") {
  RuleSet rs;
  rs.add(Rule{
      .verdict = Verdict::allow,
      .tool_pattern = "file.*",
      .capability = Capability::read_file,
  });

  constexpr std::array<Capability, 1> required{Capability::read_file};
  const auto decision = rs.evaluate("file.read", std::span<const Capability>{required}, Mode::strict);
  REQUIRE(decision.verdict == Verdict::allow);
  REQUIRE(decision.reason.find("capability=read_file") != std::string::npos);
  REQUIRE(decision.reason.find("file.*") != std::string::npos);
}

TEST_CASE("capability-bound rule does not fire when capability is missing", "[unit][permission][capability]") {
  RuleSet rs;
  rs.add(Rule{
      .verdict = Verdict::allow,
      .tool_pattern = "file.*",
      .capability = Capability::read_file,
  });

  // The call only requires `write_file`; the rule's `read_file` scope filters
  // it out and we fall back to the mode default.
  constexpr std::array<Capability, 1> required{Capability::write_file};
  const auto decision = rs.evaluate("file.read", std::span<const Capability>{required}, Mode::strict);
  REQUIRE(decision.verdict == Verdict::deny);  // strict default
  REQUIRE(decision.reason.find("default by mode=strict") != std::string::npos);
}

TEST_CASE("legacy evaluate(tool_name, mode) skips capability-bound rules", "[unit][permission][capability]") {
  RuleSet rs;
  rs.add(Rule{
      .verdict = Verdict::allow,
      .tool_pattern = "file.*",
      .capability = Capability::read_file,
  });

  // The capability-less overload behaves as if the caller passed `{}`; the
  // capability-bound rule does not fire and we fall back to the mode default.
  const auto decision = rs.evaluate("file.read", Mode::permissive);
  REQUIRE(decision.verdict == Verdict::allow);  // permissive default
  REQUIRE(decision.reason.find("default by mode=permissive") != std::string::npos);
}

TEST_CASE("capability-bound deny outranks capability-bound allow at the same scope", "[unit][permission][capability]") {
  RuleSet rs;
  rs.add(Rule{
      .verdict = Verdict::allow,
      .tool_pattern = "*",
      .capability = Capability::spawn_subprocess,
  });
  rs.add(Rule{
      .verdict = Verdict::deny,
      .tool_pattern = "*",
      .capability = Capability::runtime_loader,
  });

  constexpr std::array<Capability, 2> required{
      Capability::spawn_subprocess,
      Capability::runtime_loader,
  };
  const auto decision = rs.evaluate("shell.exec", std::span<const Capability>{required}, Mode::permissive);
  REQUIRE(decision.verdict == Verdict::deny);
  REQUIRE(decision.reason.find("capability=runtime_loader") != std::string::npos);
}

TEST_CASE("capability mismatch falls through to next precedence pass", "[unit][permission][capability]") {
  RuleSet rs;
  // A deny scoped to a capability the call does not declare: should not fire.
  rs.add(Rule{
      .verdict = Verdict::deny,
      .tool_pattern = "*",
      .capability = Capability::runtime_loader,
  });
  // An unscoped allow that should win.
  rs.add(Rule{.verdict = Verdict::allow, .tool_pattern = "shell.exec", .capability = std::nullopt});

  constexpr std::array<Capability, 1> required{Capability::spawn_subprocess};
  const auto decision = rs.evaluate("shell.exec", std::span<const Capability>{required}, Mode::strict);
  REQUIRE(decision.verdict == Verdict::allow);
  REQUIRE(decision.reason.find("rule #1") != std::string::npos);
  REQUIRE(decision.reason.find("capability=") == std::string::npos);
}

TEST_CASE("unscoped rule keeps matching when call passes capabilities", "[unit][permission][capability]") {
  RuleSet rs;
  rs.add(Rule{.verdict = Verdict::allow, .tool_pattern = "file.read", .capability = std::nullopt});

  constexpr std::array<Capability, 2> required{
      Capability::read_file,
      Capability::write_file,
  };
  const auto decision = rs.evaluate("file.read", std::span<const Capability>{required}, Mode::strict);
  REQUIRE(decision.verdict == Verdict::allow);
  // Unscoped rules omit `capability=` from the reason.
  REQUIRE(decision.reason.find("capability=") == std::string::npos);
}

TEST_CASE("capability scope round-trips in reason for every verdict", "[unit][permission][capability]") {
  RuleSet rs;
  rs.add(Rule{
      .verdict = Verdict::ask,
      .tool_pattern = "memory.*",
      .capability = Capability::write_memory,
  });

  constexpr std::array<Capability, 1> required{Capability::write_memory};
  const auto decision = rs.evaluate("memory.remember", std::span<const Capability>{required}, Mode::strict);
  REQUIRE(decision.verdict == Verdict::ask);
  REQUIRE(decision.reason.find("ask") != std::string::npos);
  REQUIRE(decision.reason.find("memory.*") != std::string::npos);
  REQUIRE(decision.reason.find("capability=write_memory") != std::string::npos);
}
