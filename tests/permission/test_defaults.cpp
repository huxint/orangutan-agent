// tests/permission/test_defaults.cpp — `Defaults::for_mode` baseline.

#include <array>
#include <span>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/capability.hpp>
#include <oran/permission.hpp>

namespace perm = orangutan::permission;
using orangutan::core::Capability;
using perm::Defaults;
using perm::Mode;
using perm::RuleSet;
using perm::Verdict;

namespace {

// Helper: evaluate `tool_name` against `rs` with exactly one required
// capability. Defaults use `*` as the tool pattern so the choice of name
// is irrelevant; only the capability matters.
[[nodiscard]] Verdict evaluate_with(const RuleSet& rs, Capability cap, Mode mode) {
  const std::array<Capability, 1> required{cap};
  return rs.evaluate("any.tool", std::span<const Capability>{required}, mode).verdict;
}

}  // namespace

TEST_CASE("Defaults::for_mode(strict) returns an empty baseline", "[unit][permission][defaults]") {
  const auto rs = Defaults::for_mode(Mode::strict);
  REQUIRE(rs.size() == 0);
  // The mode's default verdict still applies on the empty set.
  REQUIRE(evaluate_with(rs, Capability::read_file, Mode::strict) == Verdict::deny);
}

TEST_CASE("Defaults::for_mode(default_) classifies common capabilities", "[unit][permission][defaults]") {
  const auto rs = Defaults::for_mode(Mode::default_);
  REQUIRE(rs.size() == 9);

  // Allow set.
  REQUIRE(evaluate_with(rs, Capability::read_file, Mode::default_) == Verdict::allow);
  REQUIRE(evaluate_with(rs, Capability::read_memory, Mode::default_) == Verdict::allow);

  // Ask set.
  REQUIRE(evaluate_with(rs, Capability::write_file, Mode::default_) == Verdict::ask);
  REQUIRE(evaluate_with(rs, Capability::edit_file, Mode::default_) == Verdict::ask);
  REQUIRE(evaluate_with(rs, Capability::write_memory, Mode::default_) == Verdict::ask);
  REQUIRE(evaluate_with(rs, Capability::spawn_subprocess, Mode::default_) == Verdict::ask);
  REQUIRE(evaluate_with(rs, Capability::egress_http, Mode::default_) == Verdict::ask);

  // Deny set.
  REQUIRE(evaluate_with(rs, Capability::runtime_loader, Mode::default_) == Verdict::deny);
  REQUIRE(evaluate_with(rs, Capability::delete_path, Mode::default_) == Verdict::deny);

  // A capability the baseline does not mention falls back to the mode default (ask).
  REQUIRE(evaluate_with(rs, Capability::invoke_skill, Mode::default_) == Verdict::ask);
}

TEST_CASE("Defaults::for_mode(permissive) only denies the most dangerous capabilities",
          "[unit][permission][defaults]") {
  const auto rs = Defaults::for_mode(Mode::permissive);
  REQUIRE(rs.size() == 2);

  REQUIRE(evaluate_with(rs, Capability::runtime_loader, Mode::permissive) == Verdict::deny);
  REQUIRE(evaluate_with(rs, Capability::delete_path, Mode::permissive) == Verdict::deny);

  // Everything else falls back to the permissive default (allow).
  REQUIRE(evaluate_with(rs, Capability::read_file, Mode::permissive) == Verdict::allow);
  REQUIRE(evaluate_with(rs, Capability::write_file, Mode::permissive) == Verdict::allow);
  REQUIRE(evaluate_with(rs, Capability::spawn_subprocess, Mode::permissive) == Verdict::allow);
}

TEST_CASE("Defaults::for_mode(sandboxed) allows read-side capabilities only", "[unit][permission][defaults]") {
  const auto rs = Defaults::for_mode(Mode::sandboxed);
  REQUIRE(rs.size() == 2);

  REQUIRE(evaluate_with(rs, Capability::read_file, Mode::sandboxed) == Verdict::allow);
  REQUIRE(evaluate_with(rs, Capability::read_memory, Mode::sandboxed) == Verdict::allow);

  // Everything else falls back to the sandboxed default (deny).
  REQUIRE(evaluate_with(rs, Capability::write_file, Mode::sandboxed) == Verdict::deny);
  REQUIRE(evaluate_with(rs, Capability::spawn_subprocess, Mode::sandboxed) == Verdict::deny);
  REQUIRE(evaluate_with(rs, Capability::runtime_loader, Mode::sandboxed) == Verdict::deny);
}

TEST_CASE("Defaults::for_mode is referentially transparent", "[unit][permission][defaults]") {
  // Two calls produce equal-sized rule sets and classify a representative
  // call identically.
  const auto a = Defaults::for_mode(Mode::default_);
  const auto b = Defaults::for_mode(Mode::default_);
  REQUIRE(a.size() == b.size());
  REQUIRE(evaluate_with(a, Capability::read_file, Mode::default_) ==
          evaluate_with(b, Capability::read_file, Mode::default_));
  REQUIRE(evaluate_with(a, Capability::runtime_loader, Mode::default_) ==
          evaluate_with(b, Capability::runtime_loader, Mode::default_));
}

TEST_CASE("Defaults::for_mode(default_) capability-less call falls back to mode default",
          "[unit][permission][defaults]") {
  // The legacy `evaluate(tool_name, mode)` overload simulates a call that
  // carries no capability information; all rules in `Defaults::for_mode(
  // default_)` are capability-scoped, so none fire and we land on the
  // mode default (`ask`).
  const auto rs = Defaults::for_mode(Mode::default_);
  const auto decision = rs.evaluate("any.tool", Mode::default_);
  REQUIRE(decision.verdict == Verdict::ask);
  REQUIRE(decision.reason.find("default by mode=default") != std::string::npos);
}

TEST_CASE("Defaults::for_mode(strict) explicit deny is recorded with capability scope",
          "[unit][permission][defaults]") {
  // Smoke test the reason annotation flows through the factory builds.
  const auto rs = Defaults::for_mode(Mode::default_);
  const std::array<Capability, 1> required{Capability::runtime_loader};
  const auto decision = rs.evaluate("any.tool", std::span<const Capability>{required}, Mode::default_);
  REQUIRE(decision.verdict == Verdict::deny);
  REQUIRE(decision.reason.find("capability=runtime_loader") != std::string::npos);
}
