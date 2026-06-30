// src/oran-tool/path_resolution.cpp — registry-boundary workspace resolution.

#include "_impl/path_resolution.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/permission/approval.hpp>
#include <oran/permission/audit.hpp>
#include <oran/tool/builtins.hpp>
#include <oran/tool/workspace.hpp>

namespace orangutan::tool::detail {

namespace {

enum class PathIntent {
  read,
  write,
  edit,
  delete_,
  list,
};

struct PathRequest {
  PathIntent intent{PathIntent::read};
  std::string path;
  WriteIntent write_intent{};
  bool allow_outside_workspace{false};
};

[[nodiscard]] std::string hash_text(std::string_view text) {
  return permission::to_hex(permission::ApprovalAuthority::input_hash(text));
}

[[nodiscard]] std::optional<nlohmann::json> parse_object(std::string_view input_json) {
  try {
    auto parsed = nlohmann::json::parse(input_json);
    if (!parsed.is_object()) {
      return std::nullopt;
    }
    return parsed;
  } catch (const nlohmann::json::parse_error&) {
    return std::nullopt;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

[[nodiscard]] std::optional<std::string> string_field(const nlohmann::json& parsed, std::string_view field) {
  const auto it = parsed.find(std::string{field});
  if (it == parsed.end() || !it->is_string()) {
    return std::nullopt;
  }
  return it->get<std::string>();
}

[[nodiscard]] std::optional<bool> bool_field(const nlohmann::json& parsed, std::string_view field) {
  const auto it = parsed.find(std::string{field});
  if (it == parsed.end()) {
    return false;
  }
  if (!it->is_boolean()) {
    return std::nullopt;
  }
  return it->get<bool>();
}

[[nodiscard]] bool outside_override_can_apply(PathIntent intent) noexcept {
  return intent == PathIntent::read || intent == PathIntent::list;
}

[[nodiscard]] std::optional<PathRequest> path_request(std::string_view tool_name, const nlohmann::json& parsed) {
  auto path = string_field(parsed, "path");
  if (!path.has_value()) {
    return std::nullopt;
  }

  auto allow_outside_workspace = bool_field(parsed, "allow_outside_workspace");
  if (!allow_outside_workspace.has_value()) {
    return std::nullopt;
  }

  if (tool_name == kFileReadName) {
    return PathRequest{
        .intent = PathIntent::read,
        .path = std::move(*path),
        .allow_outside_workspace = *allow_outside_workspace,
    };
  }
  if (tool_name == kFileSearchName) {
    return PathRequest{
        .intent = PathIntent::list,
        .path = std::move(*path),
        .allow_outside_workspace = *allow_outside_workspace,
    };
  }
  if (tool_name == kDirectoryListName) {
    return PathRequest{
        .intent = PathIntent::list,
        .path = std::move(*path),
        .allow_outside_workspace = *allow_outside_workspace,
    };
  }
  if (tool_name == kFileDeleteName) {
    return PathRequest{.intent = PathIntent::delete_, .path = std::move(*path)};
  }
  if (tool_name == kFileEditName) {
    return PathRequest{
        .intent = PathIntent::edit,
        .path = std::move(*path),
        .write_intent = WriteIntent{.disposition = WriteDisposition::truncate},
    };
  }
  if (tool_name == kFileWriteName) {
    WriteDisposition disposition = WriteDisposition::truncate;
    if (auto it = parsed.find("mode"); it != parsed.end()) {
      if (!it->is_string()) {
        return std::nullopt;
      }
      auto parsed_mode = core::parse_enum<WriteDisposition>(it->get<std::string>());
      if (!parsed_mode.has_value()) {
        return std::nullopt;
      }
      disposition = *parsed_mode;
    }

    bool create_parents = false;
    if (auto it = parsed.find("create_parents"); it != parsed.end()) {
      if (!it->is_boolean()) {
        return std::nullopt;
      }
      create_parents = it->get<bool>();
    }

    return PathRequest{
        .intent = PathIntent::write,
        .path = std::move(*path),
        .write_intent =
            WriteIntent{
                .disposition = disposition,
                .create_parent_directories = create_parents,
            },
    };
  }

  return std::nullopt;
}

[[nodiscard]] core::Result<ResolvedPath> resolve_request(const Workspace& workspace, const PathRequest& request) {
  switch (request.intent) {
    case PathIntent::read:
      return workspace.resolve_read(request.path);
    case PathIntent::write:
    case PathIntent::edit:
      return workspace.resolve_write(request.path, request.write_intent);
    case PathIntent::delete_:
      return workspace.resolve_delete(request.path);
    case PathIntent::list:
      return workspace.resolve_list(request.path);
  }
  return std::unexpected(core::Error::internal("unknown workspace path intent"));
}

[[nodiscard]] core::Result<ResolvedPath> resolve_outside_request(const Workspace& workspace,
                                                                 const PathRequest& request) {
  switch (request.intent) {
    case PathIntent::read:
      return workspace.resolve_read_outside_workspace(request.path);
    case PathIntent::list:
      return workspace.resolve_list_outside_workspace(request.path);
    case PathIntent::write:
    case PathIntent::edit:
    case PathIntent::delete_:
      break;
  }
  return std::unexpected(core::Error::internal("outside-workspace override is not valid for this path intent"));
}

[[nodiscard]] ResolvedToolPath
to_tool_path(const Workspace& workspace, std::string_view input_path, ResolvedPath resolved) {
  auto display_path = resolved.per_call_outside_workspace_override ? resolved.absolute_path
                                                                   : workspace.display_path(resolved.absolute_path);
  return ResolvedToolPath{
      .absolute_path = std::move(resolved.absolute_path),
      .relative_path = std::move(resolved.relative_path),
      .display_path = std::move(display_path),
      .input_path_hash = hash_text(input_path),
      .workspace_root_hash = hash_text(workspace.root()),
      .symlink_followed = resolved.symlink_followed,
      .created_parents = resolved.created_parents,
      .outside_workspace_explicit_override = resolved.outside_workspace_explicit_override,
      .per_call_outside_workspace_override = resolved.per_call_outside_workspace_override,
      .override_root_index = resolved.override_root_index,
  };
}

[[nodiscard]] std::string_view context_value(const core::Error& error, std::string_view key) {
  const auto entries = error.context();
  const auto it = std::ranges::find_if(entries, [&](const auto& entry) { return entry.first == key; });
  return it == entries.end() ? std::string_view{} : std::string_view{it->second};
}

[[nodiscard]] std::string
path_resolution_error_metadata_json(const Workspace& workspace, std::string_view input_path, const core::Error& error) {
  nlohmann::json metadata = nlohmann::json::object();
  auto path_resolution = nlohmann::json::object();
  path_resolution["input_path_hash"] = hash_text(input_path);
  path_resolution["resolved_relative_path"] = nullptr;
  path_resolution["workspace_root_hash"] = hash_text(workspace.root());
  path_resolution["resolved_display_path"] = nullptr;
  path_resolution["symlink_followed"] = nullptr;
  path_resolution["created_parents"] = nullptr;
  path_resolution["outside_workspace_explicit_override"] = nullptr;
  path_resolution["per_call_outside_workspace_override"] = nullptr;
  path_resolution["override_root_index"] = nullptr;
  path_resolution["error_kind"] = std::string{core::enum_name(error.kind())};
  path_resolution["error_reason"] = std::string{context_value(error, "reason")};
  metadata["path_resolution"] = std::move(path_resolution);
  return metadata.dump();
}

}  // namespace

PathResolutionReport
pre_resolve_tool_path(std::string_view tool_name, std::string_view input_json, DispatchContext& ctx) {
  ctx.resolved_path.reset();
  if (ctx.workspace == nullptr) {
    return {};
  }

  auto parsed = parse_object(input_json);
  if (!parsed.has_value()) {
    return {};
  }

  auto request = path_request(tool_name, *parsed);
  if (!request.has_value()) {
    return {};
  }

  auto resolved = resolve_request(*ctx.workspace, *request);
  bool requires_approval = false;
  std::optional<core::Error> resolution_error;
  if (!resolved) {
    auto error = std::move(resolved).error();
    const auto reason = context_value(error, "reason");
    if (request->allow_outside_workspace && outside_override_can_apply(request->intent) &&
        (reason == "outside_workspace" || reason == "symlink_escape")) {
      auto outside_resolved = resolve_outside_request(*ctx.workspace, *request);
      if (outside_resolved) {
        resolved = std::move(outside_resolved);
        requires_approval = true;
      } else {
        error = std::move(outside_resolved).error();
      }
    }
    if (!resolved) {
      resolution_error = std::move(error);
    }
  }
  if (!resolved) {
    auto error = std::move(*resolution_error);
    return PathResolutionReport{
        .metadata_json = path_resolution_error_metadata_json(*ctx.workspace, request->path, error),
        .error = std::move(error),
    };
  }

  ctx.resolved_path = to_tool_path(*ctx.workspace, request->path, std::move(*resolved));
  return PathResolutionReport{
      .metadata_json = path_resolution_metadata_json(ctx.resolved_path),
      .requires_approval = requires_approval,
  };
}

std::string path_resolution_metadata_json(const std::optional<ResolvedToolPath>& resolved_path) {
  if (!resolved_path.has_value()) {
    return "{}";
  }

  nlohmann::json metadata = nlohmann::json::object();
  auto path_resolution = nlohmann::json::object();
  path_resolution["input_path_hash"] = resolved_path->input_path_hash;
  if (resolved_path->per_call_outside_workspace_override) {
    path_resolution["resolved_relative_path"] = nullptr;
  } else {
    path_resolution["resolved_relative_path"] = resolved_path->relative_path;
  }
  path_resolution["workspace_root_hash"] = resolved_path->workspace_root_hash;
  path_resolution["resolved_display_path"] = resolved_path->display_path;
  path_resolution["symlink_followed"] = resolved_path->symlink_followed;
  path_resolution["created_parents"] = resolved_path->created_parents;
  path_resolution["outside_workspace_explicit_override"] = resolved_path->outside_workspace_explicit_override;
  path_resolution["per_call_outside_workspace_override"] = resolved_path->per_call_outside_workspace_override;
  if (resolved_path->override_root_index.has_value()) {
    path_resolution["override_root_index"] = *resolved_path->override_root_index;
  } else {
    path_resolution["override_root_index"] = nullptr;
  }
  metadata["path_resolution"] = std::move(path_resolution);
  return metadata.dump();
}

}  // namespace orangutan::tool::detail
