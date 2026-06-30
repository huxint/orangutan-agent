// Internal workspace pre-resolution helpers for `oran-tool`.

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <oran/core/error.hpp>
#include <oran/core/result.hpp>
#include <oran/tool/registry.hpp>

namespace orangutan::tool::detail {

struct PathResolutionReport {
  std::string metadata_json{"{}"};
  std::optional<core::Error> error{};
  bool requires_approval{false};
};

/// Resolve the current call's path at the registry boundary when the call is a
/// built-in filesystem tool and `ctx.workspace` is supplied. Non-filesystem
/// tools and workspace-less calls are no-ops.
[[nodiscard]] PathResolutionReport
pre_resolve_tool_path(std::string_view tool_name, std::string_view input_json, DispatchContext& ctx);

/// Render the resolved-path audit extension. Returns "{}" when no path was
/// pre-resolved for the call.
[[nodiscard]] std::string path_resolution_metadata_json(const std::optional<ResolvedToolPath>& resolved_path);

}  // namespace orangutan::tool::detail
