// src/oran-tool/file_delete.cpp — `FileDelete` built-in.
//
// Thin wrapper around `oran-io::delete_file`. Refuses anything but a regular
// file (directories, symlinks, and unknown kinds reject with
// `invalid_argument` from the io layer) so an LLM-driven delete cannot
// recursively destroy a tree or follow a symlink outside the workspace.
// Capability `delete_path` was already in the slice-7 `core::Capability`
// vocabulary; this slice is the first built-in that actually requires it.

#include <oran/tool/builtins.hpp>

#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/capability.hpp>
#include <oran/core/error.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/io/file.hpp>
#include <oran/tool/registry.hpp>
#include <oran/tool/workspace.hpp>

#include "_impl/parse_input.hpp"

namespace orangutan::tool {

namespace {

constexpr std::string_view kFileDeleteSchema =
    R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"],"additionalProperties":false})";

[[nodiscard]] async::Awaitable<core::Result<Output>> file_delete_handler(std::string_view input_json,
                                                                         DispatchContext& ctx) {
  auto parsed = detail::parse_input_object(input_json, kFileDeleteName);
  if (!parsed) {
    co_return std::unexpected(std::move(parsed).error());
  }

  auto path_field = detail::require_string_field(*parsed, kFileDeleteName, "path");
  if (!path_field) {
    co_return std::unexpected(std::move(path_field).error());
  }

  auto path = ctx.resolved_path.has_value() ? ctx.resolved_path->absolute_path : *std::move(path_field);
  if (!ctx.resolved_path.has_value() && ctx.workspace != nullptr) {
    auto resolved = ctx.workspace->resolve_delete(path);
    if (!resolved) {
      co_return std::unexpected(std::move(resolved).error());
    }
    path = std::move(resolved->absolute_path);
  }
  auto deleted = co_await io::delete_file(ctx.executor, path);
  if (!deleted) {
    co_return std::unexpected(std::move(deleted).error());
  }
  co_return Output{
      .text = "deleted " + path,
      .usage =
          ToolUsage{
              .bytes_written = 0,
              .files_touched = 1,
          },
  };
}

}  // namespace

core::Result<void> register_file_delete(Registry& registry) {
  core::ToolDef def{
      .name = std::string{kFileDeleteName},
      .description = "Delete a regular file. Input: {\"path\": <string>}. Refuses directories and symlinks with "
                     "`invalid_argument` (the v1 surface is deliberately narrow so a recursive delete or a "
                     "symlink-follow cannot escape the workspace). Returns `not_found` when no file exists at the "
                     "path. On success returns the literal text `deleted <path>` and fills usage with "
                     "bytes_written=0 plus files_touched=1.",
      .input_schema_json = std::string{kFileDeleteSchema},
      .required_capabilities = {core::Capability::delete_path},
      .deferred = false,
      .category = "file",
  };
  return registry.add(std::move(def), &file_delete_handler);
}

}  // namespace orangutan::tool
