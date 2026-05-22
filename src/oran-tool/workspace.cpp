// src/oran-tool/workspace.cpp — workspace path policy implementation.

#include <oran/tool/workspace.hpp>

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <oran/core/error.hpp>

namespace orangutan::tool {
namespace {

using ::orangutan::core::Error;
using ::orangutan::core::Result;

struct RootMatch {
  std::filesystem::path root;
  std::optional<std::size_t> override_index{};
};

[[nodiscard]] Error path_error(std::string message, std::string_view input, std::string_view reason) {
  return Error::permission_denied(std::move(message))
      .with("path", std::string{input})
      .with("reason", std::string{reason});
}

[[nodiscard]] Error
filesystem_error(std::string message, const std::filesystem::path& path, const std::error_code& ec) {
  return Error::io(std::move(message)).with("path", path.string()).with("detail", ec.message());
}

[[nodiscard]] Result<std::filesystem::path> canonical_directory(std::string_view raw, std::string_view context_key) {
  if (raw.empty()) {
    return std::unexpected(
        Error::invalid_argument("workspace root must not be empty").with("field", std::string{context_key}));
  }

  std::error_code ec;
  auto canonical = std::filesystem::weakly_canonical(std::filesystem::path{std::string{raw}}, ec);
  if (ec) {
    return std::unexpected(
        filesystem_error("failed to canonicalize workspace root", std::filesystem::path{std::string{raw}}, ec)
            .with("field", std::string{context_key}));
  }
  canonical = canonical.lexically_normal();

  const auto status = std::filesystem::status(canonical, ec);
  if (ec) {
    return std::unexpected(
        filesystem_error("failed to inspect workspace root", canonical, ec).with("field", std::string{context_key}));
  }
  if (!std::filesystem::exists(status)) {
    return std::unexpected(Error::not_found("workspace root does not exist")
                               .with("path", canonical.string())
                               .with("field", std::string{context_key}));
  }
  if (!std::filesystem::is_directory(status)) {
    return std::unexpected(Error::invalid_argument("workspace root must be a directory")
                               .with("path", canonical.string())
                               .with("field", std::string{context_key}));
  }
  return canonical;
}

[[nodiscard]] Result<std::vector<std::string>> canonicalize_extra_roots(std::span<const std::string> roots,
                                                                        std::string_view field_name) {
  std::vector<std::string> out;
  out.reserve(roots.size());
  for (std::size_t index = 0; index < roots.size(); ++index) {
    auto field = std::string{field_name}.append("[").append(std::to_string(index)).append("]");
    auto canonical = canonical_directory(roots[index], field);
    if (!canonical) {
      return std::unexpected(std::move(canonical).error());
    }
    out.push_back(canonical->string());
  }
  return out;
}

[[nodiscard]] bool is_under_root(const std::filesystem::path& candidate, const std::filesystem::path& root) {
  const auto normalized_candidate = candidate.lexically_normal();
  const auto normalized_root = root.lexically_normal();

  auto cand_it = normalized_candidate.begin();
  auto root_it = normalized_root.begin();
  for (; root_it != normalized_root.end(); ++root_it, ++cand_it) {
    if (cand_it == normalized_candidate.end() || *cand_it != *root_it) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::filesystem::path join_input(const std::filesystem::path& root, std::string_view input) {
  auto raw = std::filesystem::path{std::string{input}};
  if (raw.is_absolute()) {
    return raw.lexically_normal();
  }
  return (root / raw).lexically_normal();
}

[[nodiscard]] bool has_symlink_component(const std::filesystem::path& candidate) {
  std::filesystem::path current;
  for (const auto& part : candidate.lexically_normal()) {
    current /= part;
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(current, ec);
    if (ec || !std::filesystem::exists(status)) {
      continue;
    }
    if (std::filesystem::is_symlink(status)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] std::optional<RootMatch> find_matching_root(const std::filesystem::path& candidate,
                                                          const std::filesystem::path& workspace_root,
                                                          std::span<const std::string> extra_roots) {
  if (is_under_root(candidate, workspace_root)) {
    return RootMatch{.root = workspace_root};
  }
  for (std::size_t index = 0; index < extra_roots.size(); ++index) {
    auto root = std::filesystem::path{extra_roots[index]};
    if (is_under_root(candidate, root)) {
      return RootMatch{.root = std::move(root), .override_index = index};
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::string relative_path(const std::filesystem::path& candidate, const std::filesystem::path& root) {
  auto relative = candidate.lexically_normal().lexically_relative(root.lexically_normal());
  if (relative.empty()) {
    return ".";
  }
  return relative.string();
}

[[nodiscard]] Result<ResolvedPath> build_resolved(const std::filesystem::path& candidate,
                                                  const RootMatch& match,
                                                  bool symlink_followed,
                                                  bool created_parents) {
  return ResolvedPath{
      .absolute_path = candidate.lexically_normal().string(),
      .relative_path = relative_path(candidate, match.root),
      .symlink_followed = symlink_followed,
      .created_parents = created_parents,
      .outside_workspace_explicit_override = match.override_index.has_value(),
      .override_root_index = match.override_index,
  };
}

[[nodiscard]] Result<std::filesystem::path>
canonical_existing_path(const std::filesystem::path& candidate, std::string_view input, std::string_view action) {
  std::error_code ec;
  const auto status = std::filesystem::symlink_status(candidate, ec);
  if (ec && status.type() != std::filesystem::file_type::not_found) {
    return std::unexpected(filesystem_error(std::string{"failed to inspect path for "}.append(action), candidate, ec));
  }
  if (!std::filesystem::exists(status)) {
    return std::unexpected(Error::not_found("workspace path does not exist").with("path", std::string{input}));
  }

  auto canonical = std::filesystem::weakly_canonical(candidate, ec);
  if (ec) {
    return std::unexpected(
        filesystem_error(std::string{"failed to canonicalize path for "}.append(action), candidate, ec));
  }
  return canonical.lexically_normal();
}

[[nodiscard]] Result<ResolvedPath> resolve_existing_readable(std::string_view input,
                                                             const std::string& root,
                                                             std::span<const std::string> extra_roots,
                                                             std::string_view action) {
  if (input.empty()) {
    return std::unexpected(Error::invalid_argument("workspace path must not be empty"));
  }

  const auto workspace_root = std::filesystem::path{root};
  const auto candidate = join_input(workspace_root, input);
  const bool symlink_followed = has_symlink_component(candidate);

  if (!find_matching_root(candidate, workspace_root, extra_roots).has_value()) {
    return std::unexpected(path_error("path is outside workspace", input, "outside_workspace"));
  }

  auto canonical = canonical_existing_path(candidate, input, action);
  if (!canonical) {
    return std::unexpected(std::move(canonical).error());
  }

  auto match = find_matching_root(*canonical, workspace_root, extra_roots);
  if (!match) {
    return std::unexpected(path_error(symlink_followed ? "path symlink escapes workspace" : "path is outside workspace",
                                      input,
                                      symlink_followed ? "symlink_escape" : "outside_workspace"));
  }

  return build_resolved(*canonical, *match, symlink_followed, false);
}

[[nodiscard]] Result<ResolvedPath> resolve_mutating(std::string_view input,
                                                    const std::string& root,
                                                    std::span<const std::string> extra_roots,
                                                    bool allow_missing_parent,
                                                    std::string_view action) {
  if (input.empty()) {
    return std::unexpected(Error::invalid_argument("workspace path must not be empty"));
  }

  const auto workspace_root = std::filesystem::path{root};
  const auto candidate = join_input(workspace_root, input);
  const bool symlink_followed = has_symlink_component(candidate);
  if (symlink_followed) {
    return std::unexpected(path_error("mutating path resolves through a symlink", input, "symlink_target"));
  }

  std::error_code ec;
  const auto status = std::filesystem::symlink_status(candidate, ec);
  if (ec && status.type() != std::filesystem::file_type::not_found) {
    return std::unexpected(filesystem_error(std::string{"failed to inspect path for "}.append(action), candidate, ec));
  }

  auto canonical = candidate.lexically_normal();
  bool created_parents = false;
  if (std::filesystem::exists(status)) {
    auto resolved = std::filesystem::weakly_canonical(candidate, ec);
    if (ec) {
      return std::unexpected(
          filesystem_error(std::string{"failed to canonicalize path for "}.append(action), candidate, ec));
    }
    canonical = resolved.lexically_normal();
  } else {
    const auto parent = candidate.parent_path();
    if (!parent.empty()) {
      const auto parent_status = std::filesystem::symlink_status(parent, ec);
      if (ec && parent_status.type() != std::filesystem::file_type::not_found) {
        return std::unexpected(
            filesystem_error(std::string{"failed to inspect parent for "}.append(action), parent, ec));
      }
      const auto parent_exists = std::filesystem::exists(parent_status);
      created_parents = !parent_exists && allow_missing_parent;
      if (parent_exists) {
        auto resolved_parent = std::filesystem::weakly_canonical(parent, ec);
        if (ec) {
          return std::unexpected(
              filesystem_error(std::string{"failed to canonicalize parent for "}.append(action), parent, ec));
        }
        canonical = (resolved_parent / candidate.filename()).lexically_normal();
      }
    }
  }

  auto match = find_matching_root(canonical, workspace_root, extra_roots);
  if (!match) {
    return std::unexpected(path_error("path is outside workspace", input, "outside_workspace"));
  }

  return build_resolved(canonical, *match, false, created_parents);
}

}  // namespace

Workspace::Workspace(std::string root,
                     std::vector<std::string> extra_read_roots,
                     std::vector<std::string> extra_write_roots)
    : root_{std::move(root)}, extra_read_roots_{std::move(extra_read_roots)},
      extra_write_roots_{std::move(extra_write_roots)} {}

core::Result<Workspace> Workspace::create(std::string_view root, WorkspaceOptions options) {
  auto canonical_root = canonical_directory(root, "workspace");
  if (!canonical_root) {
    return std::unexpected(std::move(canonical_root).error());
  }

  auto read_roots = canonicalize_extra_roots(options.extra_read_roots, "permissions.workspace.extra_read_roots");
  if (!read_roots) {
    return std::unexpected(std::move(read_roots).error());
  }
  auto write_roots = canonicalize_extra_roots(options.extra_write_roots, "permissions.workspace.extra_write_roots");
  if (!write_roots) {
    return std::unexpected(std::move(write_roots).error());
  }

  return Workspace{canonical_root->string(), std::move(*read_roots), std::move(*write_roots)};
}

core::Result<ResolvedPath> Workspace::resolve_read(std::string_view path) const {
  return resolve_existing_readable(path, root_, extra_read_roots_, "read");
}

core::Result<ResolvedPath> Workspace::resolve_list(std::string_view path) const {
  return resolve_existing_readable(path, root_, extra_read_roots_, "list");
}

core::Result<ResolvedPath> Workspace::resolve_write(std::string_view path, WriteIntent intent) const {
  return resolve_mutating(path, root_, extra_write_roots_, intent.create_parent_directories, "write");
}

core::Result<ResolvedPath> Workspace::resolve_delete(std::string_view path) const {
  return resolve_mutating(path, root_, extra_write_roots_, false, "delete");
}

}  // namespace orangutan::tool
