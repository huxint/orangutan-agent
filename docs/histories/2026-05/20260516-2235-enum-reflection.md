## [2026-05-16 22:35] | Task: reflection-backed enum<->string, drop per-enum wrappers

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code in local repository checkout`
- Linked plan: none — small, mechanical migration.

### User Query

> Use GCC 16.1 C++26 reflection to implement a generic enum<->string mapping
> and reduce hand-written code. Mid-PR follow-up from the user: "obviously
> there's no need to implement a forwarding-function layer — just use
> `core::enum_name` and `core::parse_enum` directly."

### Changes Overview

- Areas: build (`-freflection` at the project root), new `oran-core`
  reflection helper, seven enum classes migrated, all callers updated.
- Key actions:
  - Added `include/oran/core/enum_names.hpp` with `enum_name`,
    `parse_enum`, `enum_values`, and a keyword-suffix stripper so
    `Mode::default_` reflects as `"default"`.
  - Deleted the per-enum forwarding shims: `to_string_view(ErrorKind|Role|StopReason|Capability|Verdict|Mode|PermissionVerdict)`,
    `parse_role`, `parse_capability`, `parse_permission_verdict`,
    `kAllCapabilities`. Callers reach for the generic helpers directly.
  - Updated every `std::formatter` specialization for these enums to call
    `core::enum_name` directly, preserving `std::print("{}", value)`
    ergonomics without a wrapper hop.
  - Migrated all callers: `oran-permission/rule_set.cpp`,
    `oran-config/config.cpp`, `oran-storage/session_repository.cpp`, the
    full test buckets for the affected enums, and the capability
    benchmark scenario.
  - Two enums whose wire spelling deviates from the identifier
    (`bootstrap::ConfigSource` with dashes like `"built-in-defaults"` and
    `cli::CliMode` with `"single-shot"`) keep their hand-written switches;
    reflection can only produce the identifier itself.
  - Added `add_cxxflags("-freflection", { force = true })` at the project
    root in `xmake.lua`.

### Design Intent

Two reductions, not one. The first reduction is what reflection buys:
the universe of enumerators is regenerated from the type at compile
time, so adding a new enumerator to an existing `enum class` no longer
requires touching a hand-maintained string table — that whole class of
forgotten-enumerator bugs disappears mechanically. The second reduction
is the user's pushback: once a generic helper exists, the per-enum
`to_string_view(EnumKind)` wrapper is redundant overhead — a header
declaration plus a `.cpp` definition that exists only to call the
generic. Callers reach for the generic name directly. The
`std::formatter` specializations stay because they hook a stdlib
customization point that callers can't bypass.

### Files Modified

- `xmake.lua` — `-freflection` at project scope.
- `include/oran/core/enum_names.hpp` — new helper (heavy include).
- `include/oran/core/error.hpp`, `role.hpp`, `stop_reason.hpp`,
  `capability.hpp` — dropped wrapper declarations; formatter calls
  `enum_name` directly.
- `include/oran/permission/rule_set.hpp` — same.
- `include/oran/config/config.hpp` — same.
- `src/oran-core/error.cpp` — dropped wrapper body.
- `src/oran-core/role.cpp`, `stop_reason.cpp`, `capability.cpp` —
  deleted (now empty).
- `src/oran-permission/rule_set.cpp` — wrapper bodies removed, callers
  use `core::enum_name`.
- `src/oran-config/config.cpp` — wrapper bodies removed; callers use
  `core::parse_enum<...>`.
- `src/oran-storage/session_repository.cpp` — caller updated.
- `tests/core/test_role.cpp`, `test_stop_reason.cpp`,
  `test_capability.cpp`, `test_error.cpp` — call the generic helpers.
- `tests/permission/test_rule_set.cpp` — same.
- `tests/config/test_config.cpp` — same.
- `bench/core/scenarios/capability.cpp` — uses
  `core::parse_enum<Capability>` and `core::enum_values<Capability>()`;
  also flipped the unordered-map check from `find != end()` to
  `contains`.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- The rule that codifies "use `core::enum_name`/`parse_enum` directly,
  don't add per-enum wrappers" lands in the next commit alongside the
  wider rule update.
- This history entry — `docs/histories/2026-05/20260516-2235-enum-reflection.md`.

### Validation

- Commands run: `xmake build` (clean across all 8 libraries +
  `orangutan`); per-target build of every test bucket; `xmake run` for
  each test bucket.
- Tests added/changed: existing per-enum coverage rewritten in terms of
  the generic helpers (no test-count loss). 1341 assertions across 177
  test cases pass.
- Bench impact: `bench-core` rebuilt clean; not re-run in this commit
  (the reflection scan lowers to the same comparison chain the table-
  based code expanded to).
- Compile-budget delta: every TU that includes one of the migrated
  enum headers now pulls `<meta>`. Confined to `enum_names.hpp` and the
  enum headers that include it; followed up in `compile-budget.md` if
  the budget bench shows pressure.

### Follow-ups

- Issues to file: none.
- Tech-debt entry: revisit `ConfigSource` and `CliMode` if/when a
  dash-aware reflection helper proves itself across enough call sites.
- Linked release note: none (pre-release).
