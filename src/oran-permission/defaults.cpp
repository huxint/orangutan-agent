// src/oran-permission/defaults.cpp — `Defaults::for_mode` baseline factory.

#include <oran/permission/defaults.hpp>

#include <oran/core/capability.hpp>
#include <oran/permission/rule_set.hpp>

namespace orangutan::permission {

namespace {

[[nodiscard]] Rule allow(core::Capability capability) {
  return Rule{
      .verdict = Verdict::allow,
      .tool_pattern = "*",
      .capability = capability,
      .input_pattern = std::nullopt,
  };
}

[[nodiscard]] Rule ask(core::Capability capability) {
  return Rule{
      .verdict = Verdict::ask,
      .tool_pattern = "*",
      .capability = capability,
      .input_pattern = std::nullopt,
  };
}

[[nodiscard]] Rule deny(core::Capability capability) {
  return Rule{
      .verdict = Verdict::deny,
      .tool_pattern = "*",
      .capability = capability,
      .input_pattern = std::nullopt,
  };
}

[[nodiscard]] RuleSet strict_baseline() {
  return RuleSet{};
}

[[nodiscard]] RuleSet default_baseline() {
  RuleSet rs;
  rs.add(deny(core::Capability::runtime_loader));
  rs.add(deny(core::Capability::delete_path));
  rs.add(allow(core::Capability::read_file));
  rs.add(allow(core::Capability::read_memory));
  rs.add(ask(core::Capability::write_file));
  rs.add(ask(core::Capability::edit_file));
  rs.add(ask(core::Capability::write_memory));
  rs.add(ask(core::Capability::spawn_subprocess));
  rs.add(ask(core::Capability::egress_http));
  return rs;
}

[[nodiscard]] RuleSet permissive_baseline() {
  RuleSet rs;
  rs.add(deny(core::Capability::runtime_loader));
  rs.add(deny(core::Capability::delete_path));
  return rs;
}

[[nodiscard]] RuleSet sandboxed_baseline() {
  RuleSet rs;
  rs.add(allow(core::Capability::read_file));
  rs.add(allow(core::Capability::read_memory));
  return rs;
}

}  // namespace

RuleSet Defaults::for_mode(Mode mode) {
  switch (mode) {
    case Mode::strict:
      return strict_baseline();
    case Mode::default_:
      return default_baseline();
    case Mode::permissive:
      return permissive_baseline();
    case Mode::sandboxed:
      return sandboxed_baseline();
  }
  // Unknown mode (e.g. cast-from-int): empty baseline. The mode's default
  // verdict still applies; this matches the "missing case is safe" risk-
  // mitigation note in the exec plan's Risks section.
  return RuleSet{};
}

}  // namespace orangutan::permission
