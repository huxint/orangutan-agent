// src/oran-tool/path_resolution.cpp — registry-boundary workspace resolution.

#include "_impl/path_resolution.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include <oran/core/capability.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/permission/approval.hpp>
#include <oran/permission/audit.hpp>
#include <oran/tool/builtins.hpp>
#include <oran/tool/workspace.hpp>

#include "_impl/parse_input.hpp"

namespace orangutan::tool::detail {

namespace {

[[nodiscard]] std::string hash_text(std::string_view text) {
  return permission::to_hex(permission::ApprovalAuthority::input_hash(text));
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

[[nodiscard]] std::optional<LockDirection> lock_direction(std::span<const core::Capability> capabilities) {
  if (std::ranges::any_of(capabilities, [](auto cap) {
        return cap == core::Capability::write_file || cap == core::Capability::edit_file ||
               cap == core::Capability::delete_path;
      })) {
    return LockDirection::write;
  }
  if (std::ranges::any_of(capabilities, [](auto cap) {
        return cap == core::Capability::read_file || cap == core::Capability::list_directory;
      })) {
    return LockDirection::read;
  }
  return std::nullopt;
}

[[nodiscard]] bool is_filesystem_builtin(std::string_view name) {
  return name == kFileReadName || name == kFileWriteName || name == kFileEditName;
}

[[nodiscard]] std::optional<PathRequest>
path_request(std::string_view tool_name, const nlohmann::json& parsed, std::optional<LockDirection> direction) {
  auto path = require_string_field(parsed, tool_name, "path");
  if (!path.has_value()) {
    return std::nullopt;
  }

  if (!is_filesystem_builtin(tool_name)) {
    return PathRequest{.path = std::move(*path), .lock_direction = direction};
  }

  auto allow_outside_workspace = bool_field(parsed, "allow_outside_workspace");
  if (!allow_outside_workspace.has_value()) {
    return std::nullopt;
  }

  if (tool_name == kFileReadName) {
    return PathRequest{
        .path = std::move(*path),
        .lock_direction = direction,
        .intent = PathIntent::read,
        .allow_outside_workspace = *allow_outside_workspace,
    };
  }
  if (tool_name == kFileEditName) {
    return PathRequest{
        .path = std::move(*path),
        .lock_direction = direction,
        .intent = PathIntent::write,
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
        .path = std::move(*path),
        .lock_direction = direction,
        .intent = PathIntent::write,
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
      return workspace.resolve_write(request.path, request.write_intent);
    case PathIntent::none:
      break;
  }
  return std::unexpected(core::Error::internal("unknown workspace path intent"));
}

[[nodiscard]] ResolvedToolPath
to_tool_path(const Workspace& workspace, std::string_view input_path, ResolvedPath resolved) {
  auto display_path = resolved.per_call_outside_workspace_override ? resolved.absolute_path
                                                                   : workspace.display_path(resolved.absolute_path);
  return ResolvedToolPath{
      .authority = std::move(resolved.authority),
      .authority_relative_path = std::move(resolved.authority_relative_path),
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

[[nodiscard]] std::string path_resolution_metadata_json(const ResolvedToolPath& resolved_path) {
  nlohmann::json metadata = nlohmann::json::object();
  auto path_resolution = nlohmann::json::object();
  path_resolution["input_path_hash"] = resolved_path.input_path_hash;
  if (resolved_path.per_call_outside_workspace_override) {
    path_resolution["resolved_relative_path"] = nullptr;
  } else {
    path_resolution["resolved_relative_path"] = resolved_path.relative_path;
  }
  path_resolution["workspace_root_hash"] = resolved_path.workspace_root_hash;
  path_resolution["resolved_display_path"] = resolved_path.display_path;
  path_resolution["symlink_followed"] = resolved_path.symlink_followed;
  path_resolution["created_parents"] = resolved_path.created_parents;
  path_resolution["outside_workspace_explicit_override"] = resolved_path.outside_workspace_explicit_override;
  path_resolution["per_call_outside_workspace_override"] = resolved_path.per_call_outside_workspace_override;
  if (resolved_path.override_root_index.has_value()) {
    path_resolution["override_root_index"] = *resolved_path.override_root_index;
  } else {
    path_resolution["override_root_index"] = nullptr;
  }
  metadata["path_resolution"] = std::move(path_resolution);
  return metadata.dump();
}

}  // namespace

std::optional<PathRequest> prepare_tool_path(const core::ToolDef& def, std::string_view input_json) {
  const auto direction = lock_direction(def.required_capabilities);
  if (!direction && !is_filesystem_builtin(def.name)) {
    return std::nullopt;
  }
  auto parsed = parse_input_object(input_json, def.name);
  if (!parsed) {
    return std::nullopt;
  }
  return path_request(def.name, *parsed, direction);
}

PathResolutionReport resolve_tool_path(const Workspace& workspace, const PathRequest& request) {
  if (request.intent == PathIntent::none) {
    return {};
  }
  auto resolved = resolve_request(workspace, request);
  bool requires_approval = false;
  if (!resolved && request.allow_outside_workspace && request.intent == PathIntent::read) {
    const auto reason = context_value(resolved.error(), "reason");
    if (reason == "outside_workspace" || reason == "symlink_escape") {
      resolved = workspace.resolve_read_outside_workspace(request.path);
      requires_approval = resolved.has_value();
    }
  }
  if (!resolved) {
    return PathResolutionReport{
        .metadata_json = path_resolution_error_metadata_json(workspace, request.path, resolved.error()),
        .error = std::move(resolved).error(),
    };
  }

  auto path = to_tool_path(workspace, request.path, std::move(*resolved));
  auto metadata = path_resolution_metadata_json(path);
  return PathResolutionReport{
      .path = std::move(path),
      .metadata_json = std::move(metadata),
      .requires_approval = requires_approval,
  };
}

}  // namespace orangutan::tool::detail
