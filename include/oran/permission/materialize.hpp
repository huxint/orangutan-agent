// include/oran/permission/materialize.hpp — three-layer rule merge.
//
// `materialize` combines the three rule sources named in
// `docs/design-docs/permissions-and-hooks.md` ("Sources"):
//
//   1. `Defaults::for_mode(mode)` — built-in safe baseline.
//   2. The global `config.permissions` block, exposed as
//      `config::PermissionsConfig` from `oran-config`.
//   3. The per-agent overlay, exposed as
//      `config::PermissionsConfig` lifted from
//      `agents.<name>.permissions` in `oran-config`.
//
// The result is a single `RuleSet` whose rules appear in
// defaults → global → per-agent order. The runtime evaluator's
// deny → allow → ask precedence walk is unchanged, so an
// explicit `deny` in any layer outranks an `allow` in any other
// layer — matching the design doc's "explicit `deny` always
// wins over `allow`" guarantee.
//
// The function is intentionally a pure concatenation; there is
// no per-rule diff/merge here. "Later layers override earlier
// ones" in the design doc means "the precedence walk visits
// later layers' rules too", not "later layers silently drop
// earlier layers' rules". A real diff/merge would surprise
// operators reading audit logs.

#pragma once

#include <oran/config/config.hpp>
#include <oran/permission/rule_set.hpp>

namespace orangutan::permission {

/// Build the three-layer merged rule set described above.
/// `global` is the parsed `config.permissions` block;
/// `per_agent` is the parsed `agents.<name>.permissions`
/// overlay (or an empty `PermissionsConfig{}` if the caller
/// has not picked an agent).
[[nodiscard]] RuleSet
materialize(Mode mode, const config::PermissionsConfig& global, const config::PermissionsConfig& per_agent);

/// Convenience overload that uses an empty per-agent overlay.
/// Mostly for call sites that have not selected an agent yet
/// (e.g. startup smoke tests).
[[nodiscard]] RuleSet materialize(Mode mode, const config::PermissionsConfig& global);

}  // namespace orangutan::permission
