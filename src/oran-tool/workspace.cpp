// src/oran-tool/workspace.cpp — workspace path policy implementation.

#include <oran/tool/workspace.hpp>

#include <fnmatch.h>

#include <algorithm>
#include <array>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
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

constexpr std::array<std::string_view, 5> kIgnoredDirectoryNames{
    ".git",
    ".xmake",
    ".orangutan",
    "build",
    "node_modules",
};

[[nodiscard]] bool is_hidden_path(const std::filesystem::path& path) {
  const auto name = path.filename().string();
  return !name.empty() && name.front() == '.';
}

[[nodiscard]] bool is_builtin_ignored_directory(const std::filesystem::path& path) {
  const auto name = path.filename().string();
  return std::ranges::contains(kIgnoredDirectoryNames, std::string_view{name});
}

[[nodiscard]] bool is_ascii_space(char ch) noexcept {
  return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v';
}

[[nodiscard]] std::string trim_ascii(std::string_view text) {
  while (!text.empty() && is_ascii_space(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && is_ascii_space(text.back())) {
    text.remove_suffix(1);
  }
  return std::string{text};
}

struct IgnoreRule {
  std::filesystem::path base_directory;
  std::string pattern;
  bool negated{false};
  bool directory_only{false};
  bool anchored{false};
  bool contains_slash{false};
};

struct IgnoreScope {
  std::filesystem::path directory;
  std::vector<IgnoreRule> rules;
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
  // libstdc++ sets `ec = ENOENT` even though `status.type()` already
  // reports `file_not_found`; pass ENOENT through to the existence check
  // below so the caller gets `Error::not_found`, not `Error::io`.
  if (ec && ec != std::errc::no_such_file_or_directory) {
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
    auto field = std::format("{}[{}]", field_name, index);
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

[[nodiscard]] std::string relative_display_path(const std::filesystem::path& candidate,
                                                const std::filesystem::path& root) {
  auto relative = candidate.lexically_normal().lexically_relative(root.lexically_normal());
  if (relative.empty()) {
    return ".";
  }
  return relative.generic_string();
}

[[nodiscard]] std::optional<std::string> relative_generic_string(const std::filesystem::path& path,
                                                                 const std::filesystem::path& base) {
  const auto relative = path.lexically_normal().lexically_relative(base.lexically_normal());
  if (relative.empty()) {
    return std::string{"."};
  }
  for (const auto& part : relative) {
    if (part == "..") {
      return std::nullopt;
    }
  }
  return relative.generic_string();
}

[[nodiscard]] std::optional<IgnoreRule> parse_ignore_line(std::string_view raw_line,
                                                          const std::filesystem::path& base_directory) {
  auto text = trim_ascii(raw_line);
  if (text.empty()) {
    return std::nullopt;
  }
  bool escaped_leading_marker = false;
  if (text.starts_with("\\#") || text.starts_with("\\!")) {
    text.erase(text.begin());
    escaped_leading_marker = true;
  } else if (text.front() == '#') {
    return std::nullopt;
  }

  bool negated = false;
  if (!escaped_leading_marker && !text.empty() && text.front() == '!') {
    negated = true;
    text.erase(text.begin());
  }

  bool anchored = false;
  while (!text.empty() && text.front() == '/') {
    anchored = true;
    text.erase(text.begin());
  }

  bool directory_only = false;
  while (!text.empty() && text.back() == '/') {
    directory_only = true;
    text.pop_back();
  }
  if (text.empty()) {
    return std::nullopt;
  }
  const bool contains_slash = text.contains('/');

  return IgnoreRule{
      .base_directory = base_directory.lexically_normal(),
      .pattern = std::move(text),
      .negated = negated,
      .directory_only = directory_only,
      .anchored = anchored,
      .contains_slash = contains_slash,
  };
}

[[nodiscard]] std::vector<IgnoreRule> load_ignore_rules(const std::filesystem::path& directory) {
  constexpr std::array<std::string_view, 2> kIgnoreFiles{".gitignore", ".ignore"};

  std::vector<IgnoreRule> rules;
  for (const auto name : kIgnoreFiles) {
    std::ifstream input{directory / name, std::ios::binary};
    if (!input) {
      continue;
    }
    std::string line;
    while (std::getline(input, line)) {
      if (auto rule = parse_ignore_line(line, directory); rule.has_value()) {
        rules.push_back(std::move(*rule));
      }
    }
  }
  return rules;
}

[[nodiscard]] bool path_pattern_matches(std::string_view pattern, std::string_view candidate, bool path_mode) {
  const int flags = path_mode ? FNM_PATHNAME : 0;
  return fnmatch(std::string{pattern}.c_str(), std::string{candidate}.c_str(), flags) == 0;
}

[[nodiscard]] bool ignore_rule_matches(const IgnoreRule& rule, const std::filesystem::path& path, bool is_directory) {
  if (rule.directory_only && !is_directory) {
    return false;
  }

  if (rule.anchored || rule.contains_slash) {
    const auto relative = relative_generic_string(path, rule.base_directory);
    return relative.has_value() && path_pattern_matches(rule.pattern, *relative, true);
  }

  return path_pattern_matches(rule.pattern, path.filename().generic_string(), false);
}

[[nodiscard]] std::vector<std::filesystem::path> directories_to(const std::filesystem::path& root,
                                                                const std::filesystem::path& parent) {
  std::vector<std::filesystem::path> directories;
  directories.push_back(root);

  auto relative = parent.lexically_normal().lexically_relative(root);
  if (relative.empty()) {
    return directories;
  }

  auto current = root;
  for (const auto& part : relative) {
    if (part == "." || part == "..") {
      continue;
    }
    current /= part;
    directories.push_back(current.lexically_normal());
  }
  return directories;
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
      .per_call_outside_workspace_override = false,
      .override_root_index = match.override_index,
  };
}

[[nodiscard]] Result<ResolvedPath> build_per_call_outside_resolved(const std::filesystem::path& candidate,
                                                                   bool symlink_followed) {
  return ResolvedPath{
      .absolute_path = candidate.lexically_normal().string(),
      .relative_path = {},
      .symlink_followed = symlink_followed,
      .created_parents = false,
      .outside_workspace_explicit_override = true,
      .per_call_outside_workspace_override = true,
      .override_root_index = std::nullopt,
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

[[nodiscard]] Result<ResolvedPath>
resolve_existing_readable_outside_workspace(std::string_view input, const std::string& root, std::string_view action) {
  if (input.empty()) {
    return std::unexpected(Error::invalid_argument("workspace path must not be empty"));
  }

  const auto workspace_root = std::filesystem::path{root};
  const auto candidate = join_input(workspace_root, input);
  const bool symlink_followed = has_symlink_component(candidate);

  auto canonical = canonical_existing_path(candidate, input, action);
  if (!canonical) {
    return std::unexpected(std::move(canonical).error());
  }

  return build_per_call_outside_resolved(*canonical, symlink_followed);
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

struct WorkspaceWalkFilter::Impl {
  std::filesystem::path root;
  WorkspaceWalkOptions options;
  std::vector<IgnoreScope> scopes;

  explicit Impl(std::string_view raw_root, WorkspaceWalkOptions raw_options)
      : root{std::filesystem::path{std::string{raw_root}}.lexically_normal()}, options{raw_options} {}

  void sync_for_parent(const std::filesystem::path& parent) {
    const auto directories = directories_to(root, parent);
    std::size_t common = 0;
    while (common < scopes.size() && common < directories.size() && scopes[common].directory == directories[common]) {
      ++common;
    }
    scopes.resize(common);
    for (std::size_t index = common; index < directories.size(); ++index) {
      scopes.push_back(IgnoreScope{
          .directory = directories[index],
          .rules = load_ignore_rules(directories[index]),
      });
    }
  }

  [[nodiscard]] bool ignore_files_match(const std::filesystem::path& path, bool is_directory) const {
    bool ignored = false;
    for (const auto& scope : scopes) {
      for (const auto& rule : scope.rules) {
        if (ignore_rule_matches(rule, path, is_directory)) {
          ignored = !rule.negated;
        }
      }
    }
    return ignored;
  }
};

WorkspaceWalkFilter::WorkspaceWalkFilter(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}

WorkspaceWalkFilter WorkspaceWalkFilter::create(std::string_view root, WorkspaceWalkOptions options) {
  return WorkspaceWalkFilter{std::make_unique<Impl>(root, options)};
}

WorkspaceWalkFilter::WorkspaceWalkFilter(WorkspaceWalkFilter&&) noexcept = default;

WorkspaceWalkFilter& WorkspaceWalkFilter::operator=(WorkspaceWalkFilter&&) noexcept = default;

WorkspaceWalkFilter::~WorkspaceWalkFilter() = default;

bool WorkspaceWalkFilter::should_skip(std::string_view path, bool is_directory) {
  const auto entry_path = std::filesystem::path{std::string{path}}.lexically_normal();
  if (!impl_->options.include_hidden && is_hidden_path(entry_path)) {
    return true;
  }
  if (!impl_->options.respect_ignore) {
    return false;
  }
  if (is_directory && is_builtin_ignored_directory(entry_path)) {
    return true;
  }
  impl_->sync_for_parent(entry_path.parent_path());
  return impl_->ignore_files_match(entry_path, is_directory);
}

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

core::Result<ResolvedPath> Workspace::resolve_read_outside_workspace(std::string_view path) const {
  return resolve_existing_readable_outside_workspace(path, root_, "read");
}

core::Result<ResolvedPath> Workspace::resolve_list_outside_workspace(std::string_view path) const {
  return resolve_existing_readable_outside_workspace(path, root_, "list");
}

core::Result<ResolvedPath> Workspace::resolve_write(std::string_view path, WriteIntent intent) const {
  return resolve_mutating(path, root_, extra_write_roots_, intent.create_parent_directories, "write");
}

core::Result<ResolvedPath> Workspace::resolve_delete(std::string_view path) const {
  return resolve_mutating(path, root_, extra_write_roots_, false, "delete");
}

WorkspaceWalkFilter Workspace::walk_filter(std::string_view root, WorkspaceWalkOptions options) const {
  return WorkspaceWalkFilter::create(root, options);
}

std::string Workspace::display_path(std::string_view absolute_path) const {
  const auto candidate = std::filesystem::path{std::string{absolute_path}}.lexically_normal();
  const auto workspace_root = std::filesystem::path{root_};
  const auto render = [&](std::string_view label, const std::filesystem::path& root) {
    const auto relative = relative_display_path(candidate, root);
    if (relative == ".") {
      return std::string{label};
    }
    return std::format("{}/{}", label, relative);
  };

  if (is_under_root(candidate, workspace_root)) {
    return render("<workspace>", workspace_root);
  }
  for (std::size_t index = 0; index < extra_read_roots_.size(); ++index) {
    const auto root = std::filesystem::path{extra_read_roots_[index]};
    if (is_under_root(candidate, root)) {
      return render(std::format("<read-root-{}>", index), root);
    }
  }
  for (std::size_t index = 0; index < extra_write_roots_.size(); ++index) {
    const auto root = std::filesystem::path{extra_write_roots_[index]};
    if (is_under_root(candidate, root)) {
      return render(std::format("<write-root-{}>", index), root);
    }
  }
  return std::string{absolute_path};
}

}  // namespace orangutan::tool
