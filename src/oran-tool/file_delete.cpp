// src/oran-tool/file_delete.cpp — `file.delete` built-in.
//
// Thin wrapper around `oran-io::delete_file`. Refuses anything but a regular
// file (directories, symlinks, and unknown kinds reject with
// `invalid_argument` from the io layer) so an LLM-driven delete cannot
// recursively destroy a tree or follow a symlink outside the workspace.
// Capability `delete_path` was already in the slice-7 `core::Capability`
// vocabulary; this slice is the first built-in that actually requires it.

#include <oran/tool/builtins.hpp>

#include <exception>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/capability.hpp>
#include <oran/core/error.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/io/file.hpp>
#include <oran/tool/registry.hpp>
#include <oran/tool/workspace.hpp>

namespace orangutan::tool {

namespace {

constexpr std::string_view kFileDeleteSchema =
    R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"],"additionalProperties":false})";

[[nodiscard]] async::Awaitable<core::Result<Output>> file_delete_handler(std::string_view input_json,
                                                                         DispatchContext& ctx) {
  nlohmann::json parsed;
  try {
    parsed = nlohmann::json::parse(input_json);
  } catch (const nlohmann::json::parse_error& e) {
    co_return std::unexpected(
        core::Error::invalid_argument("file.delete: input is not valid JSON").with("detail", e.what()));
  } catch (const std::exception& e) {
    co_return std::unexpected(
        core::Error::invalid_argument("file.delete: input is not valid JSON").with("detail", e.what()));
  }

  if (!parsed.is_object() || !parsed.contains("path") || !parsed["path"].is_string()) {
    co_return std::unexpected(
        core::Error::invalid_argument("file.delete: input must be an object with a string `path` field"));
  }

  auto path = parsed["path"].get<std::string>();
  if (ctx.workspace != nullptr) {
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
  co_return Output{.text = "deleted " + path};
}

}  // namespace

core::Result<void> register_file_delete(Registry& registry) {
  core::ToolDef def{
      .name = std::string{kFileDeleteName},
      .description = "Delete a regular file. Input: {\"path\": <string>}. Refuses directories and symlinks with "
                     "`invalid_argument` (the v1 surface is deliberately narrow so a recursive delete or a "
                     "symlink-follow cannot escape the workspace). Returns `not_found` when no file exists at the "
                     "path. On success returns the literal text `deleted <path>`.",
      .input_schema_json = std::string{kFileDeleteSchema},
      .required_capabilities = {core::Capability::delete_path},
  };
  return registry.add(std::move(def), &file_delete_handler);
}

}  // namespace orangutan::tool
