// src/oran-tool/file_delete.cpp — `FileDelete` built-in.
//
// Executes through a pinned workspace authority. Regular files delete
// directly; directories require explicit `recursive=true`; target symlinks
// reject so an LLM-driven delete cannot follow a link outside the workspace.
// Capability `delete_path` was already in the slice-7 `core::Capability`
// vocabulary; this slice is the first built-in that actually requires it.

#include <oran/tool/builtins.hpp>

#include <cstdint>
#include <expected>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/capability.hpp>
#include <oran/core/error.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/io/directory_authority.hpp>
#include <oran/io/file.hpp>
#include <oran/tool/registry.hpp>
#include <oran/tool/workspace.hpp>

#include "_impl/parse_input.hpp"

namespace orangutan::tool {

namespace {

constexpr std::string_view kFileDeleteSchema =
    R"({"type":"object","properties":{"path":{"type":"string"},"recursive":{"type":"boolean"}},"required":["path"],"additionalProperties":false})";

struct ParsedInput {
  std::string path;
  bool recursive{false};
};

[[nodiscard]] core::Result<ParsedInput> parse_file_delete_input(std::string_view input_json) {
  auto parsed = detail::parse_input_object(input_json, kFileDeleteName);
  if (!parsed) {
    return std::unexpected(std::move(parsed).error());
  }

  auto path_field = detail::require_string_field(*parsed, kFileDeleteName, "path");
  if (!path_field) {
    return std::unexpected(std::move(path_field).error());
  }

  auto result = ParsedInput{.path = *std::move(path_field)};
  if (parsed->contains("recursive")) {
    if (!(*parsed)["recursive"].is_boolean()) {
      return std::unexpected(core::Error::invalid_argument("FileDelete: `recursive` must be a boolean"));
    }
    result.recursive = (*parsed)["recursive"].get<bool>();
  }
  return result;
}

[[nodiscard]] std::uint32_t files_touched(std::uintmax_t paths_removed) noexcept {
  constexpr auto max_touched = static_cast<std::uintmax_t>(std::numeric_limits<std::uint32_t>::max());
  return paths_removed > max_touched ? std::numeric_limits<std::uint32_t>::max()
                                     : static_cast<std::uint32_t>(paths_removed);
}

[[nodiscard]] async::Awaitable<core::Result<Output>> file_delete_handler(std::string_view input_json,
                                                                         DispatchContext& ctx) {
  auto parsed = parse_file_delete_input(input_json);
  if (!parsed) {
    co_return std::unexpected(std::move(parsed).error());
  }

  if (!ctx.resolved_path.has_value() || !ctx.resolved_path->delete_mutation.has_value()) {
    co_return std::unexpected(core::Error::internal("FileDelete requires a pre-approved delete mutation"));
  }
  auto deleted = co_await io::delete_path(ctx.executor,
                                          std::move(*ctx.resolved_path->delete_mutation),
                                          io::DeletePathOptions{.recursive = parsed->recursive});
  if (!deleted) {
    co_return std::unexpected(std::move(deleted).error());
  }
  const auto& path = ctx.resolved_path->absolute_path;
  co_return Output{
      .text = "deleted " + path,
      .usage =
          ToolUsage{
              .bytes_written = 0,
              .files_touched = files_touched(deleted->paths_removed),
          },
  };
}

}  // namespace

core::Result<void> register_file_delete(Registry& registry) {
  core::ToolDef def{
      .name = std::string{kFileDeleteName},
      .description =
          "Delete a file or, with explicit recursion intent, a directory tree. Input: "
          "{\"path\": <string>, \"recursive\"?: <bool default false>}. Directories require "
          "`recursive=true`; workspace symlink targets are refused with `permission_denied`. Returns "
          "`not_found` when no path exists. On success returns the literal text `deleted <path>` and fills usage with "
          "bytes_written=0 plus files_touched equal to removed path count.",
      .input_schema_json = std::string{kFileDeleteSchema},
      .required_capabilities = {core::Capability::delete_path},
      .deferred = false,
      .category = "file",
  };
  return registry.add(std::move(def), &file_delete_handler);
}

}  // namespace orangutan::tool
