// include/oran/permission/rule_set.hpp — permission engine surface.
//
// Slice 3 of `oran-permission`. The foundation slice owned the verdict
// vocabulary, the tool-name glob rule, the mode-driven default verdict, and
// the rule evaluator. Slice 2 added capability-aware gating. This slice
// adds runtime input-regex matching via `InputPattern` (re2), closing
// criterion 4 of `docs/product-specs/0008-permissions.md`. HMAC-signed
// approval prompts and audit logging land in later slices per the same
// product spec.
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
// No character classes, no `?`. Input regex matching is a separate axis:
// when `Rule::input_pattern` is set, the rule matches only when its
// pattern (re2, partial match) matches the call's `input` argument. The
// no-input overload of `evaluate` is equivalent to passing an empty
// string — rules with an `input_pattern` set therefore never fire on
// that path unless their pattern accepts the empty string.
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
#include <oran/core/enum_names.hpp>
#include <oran/permission/input_pattern.hpp>

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
  std::optional<core::Capability> capability{};
  /// Optional runtime regex matched against the call's `input` string
  /// (re2 partial match). When unset, the rule does not constrain the
  /// input. When set, the rule matches only when the pattern accepts
  /// the input — `Rule` is move-only as a result (re2 is non-copyable).
  std::optional<InputPattern> input_pattern{};

  friend bool operator==(const Rule&, const Rule&) = default;
};

struct Decision {
  Verdict verdict{Verdict::deny};
  /// Human-readable explanation: which rule fired, or which mode fell back.
  /// When the firing rule had a capability scope, the spelling appears in
  /// the reason (e.g. `rule #2 (allow: file.* capability=read_file)`).
  /// When the firing rule had an input_pattern, the pattern source string
  /// appears too (e.g. `rule #3 (deny: shell.exec input=~"^rm ")`).
  std::string reason;

  friend bool operator==(const Decision&, const Decision&) = default;
};

class RuleSet {
public:
  RuleSet() = default;

  void add(Rule rule);
  void clear() noexcept;
  [[nodiscard]] std::size_t size() const noexcept;

  /// Evaluate `tool_name` against the rule set without supplying call
  /// `input` or any capability information. Rules with a capability scope
  /// never fire on this path; rules with an `input_pattern` fire only if
  /// their pattern accepts the empty string. Equivalent to passing `""`
  /// and an empty span to the full overload.
  [[nodiscard]] Decision evaluate(std::string_view tool_name, Mode mode) const;

  /// Evaluate `tool_name` with the call's `required_capabilities` (typically
  /// the invoked tool's `ToolDef::requires` list). The call's `input` is
  /// treated as the empty string; rules with an `input_pattern` fire only
  /// if their pattern accepts the empty string. Use the four-argument
  /// overload when a real `input` is available.
  [[nodiscard]] Decision
  evaluate(std::string_view tool_name, std::span<const core::Capability> required_capabilities, Mode mode) const;

  /// Evaluate `tool_name` with the call's `input` and `required_capabilities`.
  /// The walk is precedence-respecting: every `deny` rule is consulted
  /// first, then every `allow` rule, then every `ask` rule. A rule's
  /// optional `capability` scope filters its match (set rules require the
  /// capability to appear in `required_capabilities`; unset rules ignore
  /// the span). A rule's optional `input_pattern` filters its match (set
  /// rules require the pattern to accept `input` as a re2 partial match;
  /// unset rules ignore `input`). The fallback verdict comes from `mode`.
  [[nodiscard]] Decision evaluate(std::string_view tool_name,
                                  std::string_view input,
                                  std::span<const core::Capability> required_capabilities,
                                  Mode mode) const;

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
    return std::formatter<std::string_view>::format(orangutan::core::enum_name(v), ctx);
  }
};

template <>
struct std::formatter<orangutan::permission::Mode> : std::formatter<std::string_view> {
  template <class FormatContext>
  auto format(orangutan::permission::Mode m, FormatContext& ctx) const {
    return std::formatter<std::string_view>::format(orangutan::core::enum_name(m), ctx);
  }
};
