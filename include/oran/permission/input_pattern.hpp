// include/oran/permission/input_pattern.hpp — runtime regex pattern for
// `Rule::input_pattern`.
//
// Closes criterion 4 of `docs/product-specs/0008-permissions.md` together
// with the matching `oran-config` wiring slice. The legacy project used
// compile-time `ctre`, which made it impossible to load operator-supplied
// patterns from config. v2 uses google/re2 (see `docs/rules/libraries.md`)
// — linear-time matching against adversarial input, with patterns loaded
// at runtime from `config.permissions.{allow,deny,ask}[*].input_pattern`.
//
// re2 is intentionally hidden from this public header (rule C6,
// `docs/rules/critical-rules.md`): only a forward declaration of
// `re2::RE2` appears here, the full header `<re2/re2.h>` lives in
// `input_pattern.cpp`. The owned `std::unique_ptr<re2::RE2>` is fine
// against an incomplete type as long as the destructor is defined out
// of line.
//
// Match semantics: `matches(input)` performs a **partial** match — true
// iff the pattern matches a substring of `input`. Anchored regexes
// (`^...$`) collapse to full-match semantics; that mirrors PCRE-style
// operator intuition and keeps simple denylists short
// (`rm ` denies anything containing "rm ", `^git push` denies only
// commands that start with that prefix).
//
// `InputPattern` is move-only because `re2::RE2` itself is neither
// copyable nor movable. Equality is defined on the **source pattern
// string**, not on the compiled NFA — two patterns that happen to
// recognize the same language are not considered equal here. That
// keeps the `Rule` operator== honest (it round-trips through config
// rule-for-rule) without ever calling into re2's internals.

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <oran/core/result.hpp>

namespace re2 {
class RE2;
}  // namespace re2

namespace orangutan::permission {

class InputPattern {
public:
  /// Compile `pattern` into a `re2::RE2`. On success returns the wrapped
  /// pattern; on failure returns `Error::invalid_argument` with the re2
  /// error message attached as `regex_error`. The compiler is configured
  /// quietly — invalid patterns do not log to stderr; the caller is
  /// expected to surface the returned error to the operator (config
  /// loaders attach the source path).
  [[nodiscard]] static core::Result<InputPattern> compile(std::string pattern);

  /// True iff the compiled regex matches a substring of `input`. Cheap
  /// (no allocation) on the success and failure path alike — re2's
  /// PartialMatch with no capture targets is the documented fast path.
  [[nodiscard]] bool matches(std::string_view input) const noexcept;

  /// The pattern string this `InputPattern` was compiled from. Useful
  /// for diagnostics and for `Rule::operator==`.
  [[nodiscard]] std::string_view pattern() const noexcept {
    return pattern_;
  }

  InputPattern(const InputPattern&) = delete;
  InputPattern& operator=(const InputPattern&) = delete;
  InputPattern(InputPattern&&) noexcept;
  InputPattern& operator=(InputPattern&&) noexcept;
  ~InputPattern();

  /// Equality on the source pattern string. See the file-level comment for
  /// the rationale (never call into re2 for equality).
  friend bool operator==(const InputPattern& lhs, const InputPattern& rhs) noexcept {
    return lhs.pattern_ == rhs.pattern_;
  }

private:
  InputPattern(std::string pattern, std::unique_ptr<re2::RE2> re) noexcept;

  std::string pattern_;
  std::unique_ptr<re2::RE2> re_;
};

}  // namespace orangutan::permission
