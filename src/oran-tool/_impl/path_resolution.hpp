// Internal workspace pre-resolution helpers for `oran-tool`.

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <oran/core/error.hpp>
#include <oran/core/result.hpp>
#include <oran/tool/registry.hpp>
#include <oran/tool/workspace.hpp>

namespace orangutan::tool::detail {

enum class PathIntent {
  none,
  read,
  write,
};

/// Parsed once from final hook input; contains no filesystem authority.
struct PathRequest {
  std::string path;
  std::optional<LockDirection> lock_direction;
  PathIntent intent{PathIntent::none};
  WriteIntent write_intent{};
  bool allow_outside_workspace{false};
};

struct PathResolutionReport {
  std::optional<ResolvedToolPath> path{};
  std::string metadata_json{"{}"};
  std::optional<core::Error> error{};
  bool requires_approval{false};
};

[[nodiscard]] std::optional<PathRequest> prepare_tool_path(const core::ToolDef& def, std::string_view input_json);

/// Resolve pinned authority only after admission. Custom tools retain their
/// capability-based lock request without acquiring built-in path authority.
[[nodiscard]] PathResolutionReport resolve_tool_path(const Workspace& workspace, const PathRequest& request);

}  // namespace orangutan::tool::detail
