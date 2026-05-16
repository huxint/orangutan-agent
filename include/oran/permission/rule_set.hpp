// include/oran/permission/rule_set.hpp — first permission engine surface.
//
// This is the foundation slice of `oran-permission`. It owns the verdict
// vocabulary, the tool-name glob rule, the mode-driven default verdict, and
// the rule evaluator. Capability gating, runtime input regex (re2), HMAC-
// signed approval prompts, and audit logging land in later slices per
// `docs/product-specs/0008-permissions.md`.
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

#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

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

  friend bool operator==(const Rule&, const Rule&) = default;
};

struct Decision {
  Verdict verdict{Verdict::deny};
  /// Human-readable explanation: which rule fired, or which mode fell back.
  std::string reason;

  friend bool operator==(const Decision&, const Decision&) = default;
};

class RuleSet {
public:
  RuleSet() = default;

  void add(Rule rule);
  void clear() noexcept;
  [[nodiscard]] std::size_t size() const noexcept;

  /// Evaluate `tool_name` against the rule set. The walk is precedence-
  /// respecting: every `deny` rule is consulted first, then every `allow`
  /// rule, then every `ask` rule. The fallback verdict comes from `mode`.
  [[nodiscard]] Decision evaluate(std::string_view tool_name, Mode mode) const;

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
