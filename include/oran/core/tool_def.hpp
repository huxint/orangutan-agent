// include/oran/core/tool_def.hpp — canonical tool declaration.
//
// `ToolDef` is the per-tool *declaration* every layer agrees on — the agent
// loop advertises it, the provider adapter ships it on the wire, the tool
// registry executes against it. The runtime/execution side (dispatch,
// permission, hook plumbing) lives in `oran-tool` once that library lands;
// this header only owns the value shape so the rest of the stack can mention
// it by name today.
//
// The JSON Schema payload stays an opaque `std::string` so `oran-core` stays
// nlohmann-free (rule C6); validation belongs to `oran-tool` and
// `oran-provider` adapters.

#pragma once

#include <string>
#include <vector>

#include <oran/core/capability.hpp>

namespace orangutan::core {

struct ToolDef {
  /// Stable identifier callers reference. Required.
  std::string name;
  /// Human-readable description; surfaced to the model and to the user.
  std::string description;
  /// JSON Schema describing accepted inputs. Opaque at this layer; consumers
  /// in `oran-provider` / `oran-tool` parse and validate when they need to.
  std::string input_schema_json;
  /// Capabilities the tool needs to run. The dispatcher passes this list to
  /// `permission::RuleSet::evaluate` so capability-scoped rules (`Rule::capability`)
  /// fire only when the invoked tool actually declared the capability. Spelled
  /// `required_capabilities` because `requires` is a reserved C++20 keyword;
  /// the design doc's verbatim `requires` field is realized here under this
  /// name (see `docs/design-docs/tool-runtime.md`).
  std::vector<Capability> required_capabilities;

  friend bool operator==(const ToolDef&, const ToolDef&) = default;

  /// Fixture/test convenience for parameter-less tools. Production callers
  /// should construct `ToolDef{...}` with a real schema directly so the
  /// schema shape is not silently lost.
  [[nodiscard]] static ToolDef with_no_input(std::string name, std::string description);
};

}  // namespace orangutan::core
