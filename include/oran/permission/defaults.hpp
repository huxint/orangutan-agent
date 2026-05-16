// include/oran/permission/defaults.hpp — built-in permission baseline.
//
// `Defaults::for_mode(Mode)` returns the safe baseline `RuleSet`
// described in `docs/design-docs/permissions-and-hooks.md` ("Sources":
// "Rules come from three layers, merged at runtime: 1. Built-in defaults
// — safe baseline; 2. Global config; 3. Per-agent overlay."). This
// slice owns layer 1; the runtime merge that combines all three layers
// lives on the config-wiring slice once `oran-config` knows how to
// materialize a `RuleSet`.
//
// Rules in the baseline are scoped to `core::Capability` rather than
// tool names. That follows the design-doc guidance — "match by
// capability not tool name — survives tool renames" — and keeps the
// baselines small (one rule per capability rather than per tool).

#pragma once

#include <oran/permission/rule_set.hpp>

namespace orangutan::permission {

struct Defaults {
  /// Return a baseline `RuleSet` appropriate for the given `mode`. The
  /// per-mode shape is:
  ///
  ///   `Mode::strict`     — empty baseline (strict mode denies by
  ///                        default; operators allow what they need).
  ///   `Mode::default_`   — allow `read_file`, `read_memory`;
  ///                        ask `write_file`, `edit_file`,
  ///                        `write_memory`, `spawn_subprocess`,
  ///                        `egress_http`; deny `runtime_loader`,
  ///                        `delete_path`.
  ///   `Mode::permissive` — deny `runtime_loader`, `delete_path` only
  ///                        (mode's default verdict allows the rest).
  ///   `Mode::sandboxed`  — allow `read_file`, `read_memory` only
  ///                        (mode's default verdict denies the rest).
  ///
  /// Callers layer config-driven and per-agent rules on top of this
  /// baseline; the precedence walk (`deny` first, then `allow`, then
  /// `ask`) makes an explicit `deny` in a later layer outrank an
  /// `allow` here even though both are technically "first match".
  [[nodiscard]] static RuleSet for_mode(Mode mode);
};

}  // namespace orangutan::permission
