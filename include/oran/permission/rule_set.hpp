// include/oran/permission/rule_set.hpp — permission engine surface.
//
// Slice 2 of `oran-permission`. The foundation slice owned the verdict
// vocabulary, the tool-name glob rule, the mode-driven default verdict, and
// the rule evaluator. This slice adds capability-aware gating: a rule may
// scope to a `core::Capability`, in which case it matches only when the
// invocation's required-capability list contains it. HMAC-signed approval
// prompts, runtime input regex (re2), and audit logging land in later
// slices per `docs/product-specs/0008-permissions.md`.
//
// The evaluator implements the precedence in
// `docs/design-docs/permissions-and-hooks.md`:
//
//   1. explicit `deny` first wins;
//   2. then explicit `allow` (first match);
//   3. then explicit `ask`  (first match);
//   4. otherwise the verdict is determined by `Mode`.
//
// Tool-name matching is a textbook glob with one wildcard:
//
//   `*` — matches any (possibly empty) sequence of characters
//
// No character classes, no `?`. Future runtime-regex matching lives on
// `InputPattern` (a separate slice) and does not change this surface.
//
// Capability matching is set membership: a rule whose `capability` field
// is unset matches every invocation; a rule with `capability == X` matches
// only when `X` appears in the `required_capabilities` span the caller
// passed to `evaluate`. The capability-less `evaluate(tool_name, mode)`
// overload behaves as if the caller passed an empty span, so any rule
// with a capability scope simply does not fire — that mirrors the design-
// doc semantics ("a tool that didn't declare `Capability::network` cannot
// use it even if a rule otherwise allowed").

#pragma once

#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <oran/core/capability.hpp>

namespace orangutan::permission {

enum class Verdict : std::uint8_t {
  allow,
  deny,
  ask,
};

enum class Mode : std::uint8_t {
  /// Deny everything not explicitly allowed.
  strict,
  /// Allow read-side tools by default; force `ask` for unmatched calls so
  /// the human stays in the loop on write-side surprises. Matches the
  /// "default for tools not matched by any rule = ask" row of the design
  /// doc when no rule fires.
  default_,
  /// Permissive baseline: unmatched calls allowed.
  permissive,
  /// Sandbox: deny everything not explicitly allowed, no `ask`.
  sandboxed,
};

[[nodiscard]] std::string_view to_string_view(Verdict) noexcept;
[[nodiscard]] std::string_view to_string_view(Mode) noexcept;

struct Rule {
  Verdict verdict{Verdict::deny};
  /// Glob pattern matched against the tool name. `*` matches any (possibly
  /// empty) byte sequence; everything else matches literally. Examples:
  /// `file.read`, `file.*`, `shell.exec`.
  std::string tool_pattern;
  /// Optional capability scope. When unset, the rule matches every
  /// invocation that satisfies `tool_pattern`. When set, the rule matches
  /// only when the invocation's `required_capabilities` list contains
  /// the same `core::Capability` — i.e. the tool declared it needs that
  /// capability via `ToolDef::requires` (future) or the caller injected
  /// the capability through some other path.
  std::optional<core::Capability> capability;

  friend bool operator==(const Rule&, const Rule&) = default;
};

struct Decision {
  Verdict verdict{Verdict::deny};
  /// Human-readable explanation: which rule fired, or which mode fell back.
  /// When the firing rule had a capability scope, the spelling appears in
  /// the reason (e.g. `rule #2 (allow: file.* capability=read_file)`).
  std::string reason;

  friend bool operator==(const Decision&, const Decision&) = default;
};

class RuleSet {
public:
  RuleSet() = default;

  void add(Rule rule);
  void clear() noexcept;
  [[nodiscard]] std::size_t size() const noexcept;

  /// Evaluate `tool_name` against the rule set without supplying any
  /// capability information. Rules with a capability scope never fire on
  /// this path; rules without a capability scope fire as usual. Equivalent
  /// to passing an empty span to the capability-aware overload.
  [[nodiscard]] Decision evaluate(std::string_view tool_name, Mode mode) const;

  /// Evaluate `tool_name` with the call's `required_capabilities` (typically
  /// the invoked tool's `ToolDef::requires` list). The walk is precedence-
  /// respecting: every `deny` rule is consulted first, then every `allow`
  /// rule, then every `ask` rule. A rule's optional `capability` scope
  /// filters its match — set rules require the capability to appear in
  /// `required_capabilities`; unset rules ignore the span. The fallback
  /// verdict comes from `mode`.
  [[nodiscard]] Decision
  evaluate(std::string_view tool_name, std::span<const core::Capability> required_capabilities, Mode mode) const;

private:
  std::vector<Rule> rules_;
};

/// True iff `pattern` (a `*`-glob) matches `text` byte-for-byte. Exposed so
/// callers / tests can reach for the same matcher the rule set uses.
[[nodiscard]] bool glob_match(std::string_view pattern, std::string_view text) noexcept;

}  // namespace orangutan::permission

template <>
struct std::formatter<orangutan::permission::Verdict> : std::formatter<std::string_view> {
  template <class FormatContext>
  auto format(orangutan::permission::Verdict v, FormatContext& ctx) const {
    return std::formatter<std::string_view>::format(orangutan::permission::to_string_view(v), ctx);
  }
};

template <>
struct std::formatter<orangutan::permission::Mode> : std::formatter<std::string_view> {
  template <class FormatContext>
  auto format(orangutan::permission::Mode m, FormatContext& ctx) const {
    return std::formatter<std::string_view>::format(orangutan::permission::to_string_view(m), ctx);
  }
};
