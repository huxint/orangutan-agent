// src/oran-permission/rule_set.cpp — permission engine implementation.

#include <oran/permission/rule_set.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <oran/core/capability.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/permission/input_pattern.hpp>

namespace orangutan::permission {

namespace {

[[nodiscard]] Verdict mode_default_verdict(Mode mode) noexcept {
  switch (mode) {
    case Mode::strict:
    case Mode::sandboxed:
      return Verdict::deny;
    case Mode::default_:
      return Verdict::ask;
    case Mode::permissive:
      return Verdict::allow;
  }
  return Verdict::deny;
}

[[nodiscard]] bool capability_in_set(core::Capability needle, std::span<const core::Capability> haystack) noexcept {
  return std::ranges::contains(haystack, needle);
}

[[nodiscard]] std::string format_reason(std::size_t index, const Rule& rule) {
  const auto verdict_name = core::enum_name(rule.verdict);
  std::string out = std::format("rule #{} ({}: {}", index, verdict_name, rule.tool_pattern);
  if (rule.capability.has_value()) {
    out += std::format(" capability={}", core::enum_name(*rule.capability));
  }
  if (rule.input_pattern.has_value()) {
    out += std::format(" input=~\"{}\"", rule.input_pattern->pattern());
  }
  out += ')';
  return out;
}

}  // namespace

bool glob_match(std::string_view pattern, std::string_view text) noexcept {
  // Iterative two-pointer glob matcher with backtracking on `*`. Standard
  // textbook algorithm: O(|pattern| + |text|) in the common case and
  // O(|pattern| * |text|) worst case for adversarial inputs (an acceptable
  // cost at the size of a tool name).
  std::size_t pi = 0;
  std::size_t ti = 0;
  std::size_t star = std::string_view::npos;
  std::size_t match = 0;
  while (ti < text.size()) {
    if (pi < pattern.size() && pattern[pi] == '*') {
      star = pi++;
      match = ti;
    } else if (pi < pattern.size() && pattern[pi] == text[ti]) {
      ++pi;
      ++ti;
    } else if (star != std::string_view::npos) {
      pi = star + 1;
      ++match;
      ti = match;
    } else {
      return false;
    }
  }
  while (pi < pattern.size() && pattern[pi] == '*') {
    ++pi;
  }
  return pi == pattern.size();
}

void RuleSet::add(Rule rule) {
  rules_.push_back(std::move(rule));
}

void RuleSet::clear() noexcept {
  rules_.clear();
}

std::size_t RuleSet::size() const noexcept {
  return rules_.size();
}

Decision RuleSet::evaluate(std::string_view tool_name, Mode mode) const {
  return evaluate(tool_name, std::string_view{}, std::span<const core::Capability>{}, mode);
}

Decision RuleSet::evaluate(std::string_view tool_name,
                           std::span<const core::Capability> required_capabilities,
                           Mode mode) const {
  return evaluate(tool_name, std::string_view{}, required_capabilities, mode);
}

Decision RuleSet::evaluate(std::string_view tool_name,
                           std::string_view input,
                           std::span<const core::Capability> required_capabilities,
                           Mode mode) const {
  const auto first_match = [&](Verdict want) -> std::size_t {
    for (std::size_t i = 0; i < rules_.size(); ++i) {
      const auto& rule = rules_[i];
      if (rule.verdict != want) {
        continue;
      }
      if (!glob_match(rule.tool_pattern, tool_name)) {
        continue;
      }
      if (rule.capability.has_value() && !capability_in_set(*rule.capability, required_capabilities)) {
        continue;
      }
      if (rule.input_pattern.has_value() && !rule.input_pattern->matches(input)) {
        continue;
      }
      return i;
    }
    return std::string_view::npos;
  };

  if (const auto idx = first_match(Verdict::deny); idx != std::string_view::npos) {
    const auto& rule = rules_[idx];
    return Decision{
        .verdict = Verdict::deny,
        .reason = format_reason(idx, rule),
        .replay_max = rule.replay_max,
        .approval_ttl = rule.approval_ttl,
    };
  }
  if (const auto idx = first_match(Verdict::allow); idx != std::string_view::npos) {
    const auto& rule = rules_[idx];
    return Decision{
        .verdict = Verdict::allow,
        .reason = format_reason(idx, rule),
        .replay_max = rule.replay_max,
        .approval_ttl = rule.approval_ttl,
    };
  }
  if (const auto idx = first_match(Verdict::ask); idx != std::string_view::npos) {
    const auto& rule = rules_[idx];
    return Decision{
        .verdict = Verdict::ask,
        .reason = format_reason(idx, rule),
        .replay_max = rule.replay_max,
        .approval_ttl = rule.approval_ttl,
    };
  }
  const auto fallback = mode_default_verdict(mode);
  return Decision{
      .verdict = fallback,
      .reason = std::format("default by mode={}", core::enum_name(mode)),
  };
}

}  // namespace orangutan::permission
