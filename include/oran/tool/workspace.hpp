// include/oran/tool/workspace.hpp — workspace path resolver for file tools.
//
// `Workspace` is the tool-layer policy object that turns LLM-authored path
// strings into canonical filesystem paths before a built-in calls `oran-io`.
// The public header deliberately stays `<filesystem>`-free; callers receive
// UTF-8 strings while the implementation owns the heavy path operations.

#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <oran/core/result.hpp>

namespace orangutan::tool {

struct WorkspaceOptions {
  std::vector<std::string> extra_read_roots{};
  std::vector<std::string> extra_write_roots{};
};

enum class WriteDisposition {
  truncate,
  append,
  fail_if_exists,
};

struct WriteIntent {
  WriteDisposition disposition{WriteDisposition::truncate};
  bool create_parent_directories{false};
};

struct ResolvedPath {
  /// Canonical absolute path to pass to `oran-io`.
  std::string absolute_path;
  /// Path relative to the matching root, for audit/display metadata.
  std::string relative_path;
  /// True when an existing symlink was encountered during resolution.
  bool symlink_followed{false};
  /// True when `resolve_write` accepted a missing parent because the caller
  /// explicitly allowed parent creation.
  bool created_parents{false};
  /// True when the match came from an extra root instead of the workspace root.
  bool outside_workspace_explicit_override{false};
  /// Zero-based index inside the matching extra-root list; `nullopt` for the
  /// primary workspace root.
  std::optional<std::size_t> override_root_index{};
};

class Workspace {
public:
  [[nodiscard]] static core::Result<Workspace> create(std::string_view root, WorkspaceOptions options = {});

  [[nodiscard]] std::string_view root() const noexcept {
    return root_;
  }
  [[nodiscard]] std::span<const std::string> extra_read_roots() const noexcept {
    return std::span<const std::string>{extra_read_roots_};
  }
  [[nodiscard]] std::span<const std::string> extra_write_roots() const noexcept {
    return std::span<const std::string>{extra_write_roots_};
  }

  [[nodiscard]] core::Result<ResolvedPath> resolve_read(std::string_view path) const;
  [[nodiscard]] core::Result<ResolvedPath> resolve_list(std::string_view path) const;
  [[nodiscard]] core::Result<ResolvedPath> resolve_write(std::string_view path, WriteIntent intent) const;
  [[nodiscard]] core::Result<ResolvedPath> resolve_delete(std::string_view path) const;

private:
  std::string root_;
  std::vector<std::string> extra_read_roots_;
  std::vector<std::string> extra_write_roots_;

  Workspace(std::string root, std::vector<std::string> extra_read_roots, std::vector<std::string> extra_write_roots);
};

}  // namespace orangutan::tool
