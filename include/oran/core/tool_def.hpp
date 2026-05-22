// include/oran/core/tool_def.hpp — canonical tool declaration.
//
// `ToolDef` is the per-tool *declaration* every layer agrees on — the agent
// loop advertises it, the provider adapter ships it on the wire, and the
// tool registry executes against it. Prompt-facing metadata (`deferred`,
// `category`) lives here too so renderers can build stable catalog bytes from
// a plain value snapshot. The runtime/execution side (dispatch, permission,
// hook plumbing) lives in `oran-tool`; this header owns the declaration shape
// so the rest of the stack can mention it without pulling in registry code.
//
// The JSON Schema payload stays an opaque `std::string` so `oran-core` stays
// nlohmann-free (rule C6); `oran-tool::Registry::add` performs registration
// sanity checks and provider adapters own vendor-specific projection.

#pragma once

#include <optional>
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
  /// Prompt/catalog policy bit. Deferred tools stay callable through the
  /// registry but render as name+description in the deferred-tool index until
  /// the agent promotes them for a later turn.
  bool deferred{false};
  /// Optional grouping label for UIs and prompt renderers. Kept as plain text
  /// at this layer; prompt/catalog renderers own any sorting or display shape.
  std::optional<std::string> category{};

  friend bool operator==(const ToolDef&, const ToolDef&) = default;

  /// Fixture/test convenience for parameter-less tools. Production callers
  /// should construct `ToolDef{...}` with a real schema directly so the
  /// schema shape is not silently lost.
  [[nodiscard]] static ToolDef with_no_input(std::string name, std::string description);
};

}  // namespace orangutan::core
