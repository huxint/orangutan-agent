// src/oran-tool/directory_list.cpp — `DirectoryList` built-in.
//
// Thin formatter on top of `oran-io::list_directory`: parses the JSON input,
// validates the typed options, dispatches the existing coroutine helper, and
// renders one `path:kind:size_bytes` line per entry. The default shape stays
// single-level through `oran-io::list_directory`; `recursive=true` takes the
// project-list path in this TU so the registry/workspace boundary remains
// unchanged while callers can inspect a whole tree. Slice 29 ships the
// registrar alongside the slice-17 / 18 / 19 / 20 file-tool catalog. Slice 40
// routes the input path through `tool::Workspace::resolve_list` when
// `DispatchContext::workspace` is supplied so the listing cannot escape the
// workspace via traversal or a root-side symlink; the underlying
// `oran-io::list_directory` semantics are unchanged.
//
// Slice 64 (2026-05-24) closes spec 0014's third built-in migration step
// (after slice 62 `FileRead` and slice 63 `FileSearch`): successful calls
// keep the existing `<path>:<kind>:<size_bytes or '-'>` text rendering and
// also fill `Output::data_json` with `{kind:"directory_list", path,
// include_hidden, recursive, max_entries, entry_count, entries[]}`, where
// each entry carries `{name, path, kind, size_bytes}`. `Output::usage` is
// filled with `files_touched=1` for single-level listings, root-plus-entry
// count for recursive listings, and `match_count` (entry count) so audit
// fan-out and the future scheduler can see directory-walk cost without
// parsing prose. Slice 266 moves recursive filtering into
// `WorkspaceWalkFilter`, so recursive listings now share `FileSearch`'s
// hidden-name, built-in low-signal directory, and `.gitignore` / `.ignore`
// decisions.

#include <oran/tool/builtins.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/cancellation_type.hpp>
#include <asio/post.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <nlohmann/json.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/capability.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/io/file.hpp>
#include <oran/tool/registry.hpp>
#include <oran/tool/workspace.hpp>

#include "_impl/parse_input.hpp"

namespace orangutan::tool {

namespace {

constexpr std::string_view kDirectoryListSchema =
    R"({"type":"object","properties":{"path":{"type":"string"},"include_hidden":{"type":"boolean"},)"
    R"("recursive":{"type":"boolean"},)"
    R"("max_entries":{"type":"integer","minimum":1}},"required":["path"],"additionalProperties":false})";

constexpr std::size_t kDirectoryListDefaultMax = 256;

struct ParsedInput {
  std::string path;
  bool include_hidden{false};
  bool recursive{false};
  std::size_t max_entries{kDirectoryListDefaultMax};
  const Workspace* workspace{nullptr};
};

[[nodiscard]] core::Result<ParsedInput> parse_input(std::string_view input_json) {
  auto parsed = detail::parse_input_object(input_json, kDirectoryListName);
  if (!parsed) {
    return std::unexpected(std::move(parsed).error());
  }

  auto path_field = detail::require_string_field(*parsed, kDirectoryListName, "path");
  if (!path_field) {
    return std::unexpected(std::move(path_field).error());
  }

  ParsedInput result;
  result.path = *std::move(path_field);

  if (parsed->contains("include_hidden")) {
    if (!(*parsed)["include_hidden"].is_boolean()) {
      return std::unexpected(core::Error::invalid_argument("DirectoryList: `include_hidden` must be a boolean"));
    }
    result.include_hidden = (*parsed)["include_hidden"].get<bool>();
  }

  if (parsed->contains("recursive")) {
    if (!(*parsed)["recursive"].is_boolean()) {
      return std::unexpected(core::Error::invalid_argument("DirectoryList: `recursive` must be a boolean"));
    }
    result.recursive = (*parsed)["recursive"].get<bool>();
  }

  if (parsed->contains("max_entries")) {
    if (!(*parsed)["max_entries"].is_number_unsigned()) {
      return std::unexpected(core::Error::invalid_argument("DirectoryList: `max_entries` must be a positive integer"));
    }
    const auto raw = (*parsed)["max_entries"].get<std::uint64_t>();
    if (raw == 0) {
      return std::unexpected(core::Error::invalid_argument("DirectoryList: `max_entries` must be greater than zero"));
    }
    result.max_entries = static_cast<std::size_t>(raw);
  }

  return result;
}

[[nodiscard]] bool is_cancelled(const asio::cancellation_state& cancellation) noexcept {
  return cancellation.cancelled() != asio::cancellation_type::none;
}

[[nodiscard]] io::DirectoryEntryKind classify(const std::filesystem::file_status& status) noexcept {
  if (std::filesystem::is_symlink(status)) {
    return io::DirectoryEntryKind::symlink;
  }
  if (std::filesystem::is_regular_file(status)) {
    return io::DirectoryEntryKind::regular_file;
  }
  if (std::filesystem::is_directory(status)) {
    return io::DirectoryEntryKind::directory;
  }
  return io::DirectoryEntryKind::other;
}

[[nodiscard]] std::optional<std::uintmax_t> regular_file_size(const std::filesystem::directory_entry& entry) {
  std::error_code ec;
  if (!entry.is_regular_file(ec) || ec) {
    return std::nullopt;
  }
  const auto size = entry.file_size(ec);
  if (ec) {
    return std::nullopt;
  }
  return size;
}

[[nodiscard]] core::Error entry_limit_exceeded(const ParsedInput& parsed) {
  return core::Error::io("directory entry limit exceeded")
      .with("path", parsed.path)
      .with("max_entries", std::to_string(parsed.max_entries));
}

[[nodiscard]] std::uint32_t recursive_files_touched(std::size_t entry_count) noexcept {
  if (entry_count >= std::numeric_limits<std::uint32_t>::max()) {
    return std::numeric_limits<std::uint32_t>::max();
  }
  return static_cast<std::uint32_t>(entry_count + 1U);
}

[[nodiscard]] std::string display_path(const ParsedInput& parsed, const std::filesystem::path& path) {
  if (parsed.workspace == nullptr) {
    return path.string();
  }
  return parsed.workspace->display_path(path.string());
}

void apply_display_paths(const ParsedInput& parsed, std::vector<io::DirectoryEntry>& entries) {
  if (parsed.workspace == nullptr) {
    return;
  }
  for (auto& entry : entries) {
    entry.path = parsed.workspace->display_path(entry.path);
  }
}

[[nodiscard]] core::Result<std::vector<io::DirectoryEntry>>
list_directory_recursive(const ParsedInput& parsed, const asio::cancellation_state& cancellation) {
  if (parsed.max_entries == 0) {
    return std::unexpected(
        core::Error::invalid_argument("max_entries must be greater than zero").with("path", parsed.path));
  }

  try {
    const std::filesystem::path root{parsed.path};
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
      if (ec) {
        return std::unexpected(
            core::Error::io("failed to stat directory").with("path", parsed.path).with("system_error", ec.message()));
      }
      return std::unexpected(core::Error::not_found("directory does not exist").with("path", parsed.path));
    }
    if (!std::filesystem::is_directory(root, ec)) {
      if (ec) {
        return std::unexpected(core::Error::io("failed to inspect directory type")
                                   .with("path", parsed.path)
                                   .with("system_error", ec.message()));
      }
      return std::unexpected(core::Error::invalid_argument("path is not a directory").with("path", parsed.path));
    }

    std::vector<io::DirectoryEntry> entries;
    const auto walk_options = std::filesystem::directory_options::skip_permission_denied;
    const std::filesystem::recursive_directory_iterator end{};
    auto it = std::filesystem::recursive_directory_iterator{root, walk_options};
    auto walk_filter = WorkspaceWalkFilter::create(root.string(),
                                                   WorkspaceWalkOptions{
                                                       .include_hidden = parsed.include_hidden,
                                                       .respect_ignore = true,
                                                   });
    while (it != end) {
      if (is_cancelled(cancellation)) {
        return std::unexpected(core::Error::cancelled());
      }

      const auto& entry = *it;
      const auto name = entry.path().filename().string();
      std::error_code status_ec;
      const auto status = entry.symlink_status(status_ec);
      if (status_ec) {
        return std::unexpected(core::Error::io("failed to inspect directory entry")
                                   .with("path", entry.path().string())
                                   .with("system_error", status_ec.message()));
      }
      const auto kind = classify(status);
      const auto is_directory = kind == io::DirectoryEntryKind::directory;

      if (kind == io::DirectoryEntryKind::symlink) {
        it.disable_recursion_pending();
        ++it;
        continue;
      }

      if (walk_filter.should_skip(entry.path().string(), is_directory)) {
        if (is_directory) {
          it.disable_recursion_pending();
        }
        ++it;
        continue;
      }
      if (entries.size() >= parsed.max_entries) {
        return std::unexpected(entry_limit_exceeded(parsed));
      }

      entries.push_back(io::DirectoryEntry{
          .name = name,
          .path = display_path(parsed, entry.path()),
          .kind = kind,
          .size_bytes = kind == io::DirectoryEntryKind::regular_file ? regular_file_size(entry) : std::nullopt,
      });
      ++it;
    }

    std::ranges::sort(entries, {}, &io::DirectoryEntry::path);
    return entries;
  } catch (const std::filesystem::filesystem_error& e) {
    return std::unexpected(core::Error::io("filesystem directory listing failed")
                               .with("path", parsed.path)
                               .with("system_error", e.code().message()));
  } catch (const std::exception& e) {
    return std::unexpected(
        core::Error::io("directory listing failed").with("path", parsed.path).with("exception", e.what()));
  }
}

[[nodiscard]] std::string render(const std::vector<io::DirectoryEntry>& entries) {
  if (entries.empty()) {
    return "no entries";
  }
  std::string out;
  out.reserve(entries.size() * 80U);
  for (const auto& entry : entries) {
    if (!out.empty()) {
      out.push_back('\n');
    }
    if (entry.size_bytes.has_value()) {
      std::format_to(std::back_inserter(out), "{}:{}:{}", entry.path, core::enum_name(entry.kind), *entry.size_bytes);
    } else {
      std::format_to(std::back_inserter(out), "{}:{}:-", entry.path, core::enum_name(entry.kind));
    }
  }
  return out;
}

/// Build the structured `Output::data_json` payload that mirrors the text
/// rendering but exposes a typed entries array. `size_bytes` is a JSON
/// integer for regular files and JSON null otherwise — the text path uses
/// the literal `-` for the same case, but null is the natural JSON shape so
/// callers do not have to parse a sentinel.
[[nodiscard]] std::string
format_data_json(std::string_view path, const ParsedInput& parsed, const std::vector<io::DirectoryEntry>& entries) {
  nlohmann::json entry_array = nlohmann::json::array();
  for (const auto& entry : entries) {
    nlohmann::json size_value = nullptr;
    if (entry.size_bytes.has_value()) {
      size_value = *entry.size_bytes;
    }
    entry_array.push_back(nlohmann::json{
        {"name", entry.name},
        {"path", entry.path},
        {"kind", core::enum_name(entry.kind)},
        {"size_bytes", std::move(size_value)},
    });
  }

  return nlohmann::json{
      {"kind", "directory_list"},
      {"path", std::string{path}},
      {"include_hidden", parsed.include_hidden},
      {"recursive", parsed.recursive},
      {"max_entries", parsed.max_entries},
      {"entry_count", entries.size()},
      {"entries", std::move(entry_array)},
  }
      .dump();
}

[[nodiscard]] async::Awaitable<core::Result<Output>> directory_list_handler(std::string_view input_json,
                                                                            DispatchContext& ctx) {
  auto parsed = parse_input(input_json);
  if (!parsed) {
    co_return std::unexpected(std::move(parsed).error());
  }
  parsed->workspace = ctx.workspace;

  if (ctx.resolved_path.has_value()) {
    parsed->path = ctx.resolved_path->absolute_path;
  } else if (ctx.workspace != nullptr) {
    auto resolved = ctx.workspace->resolve_list(parsed->path);
    if (!resolved) {
      co_return std::unexpected(std::move(resolved).error());
    }
    parsed->path = std::move(resolved->absolute_path);
  }

  const auto resolved_path = parsed->path;
  core::Result<std::vector<io::DirectoryEntry>> entries;
  if (parsed->recursive) {
    auto cancellation = co_await asio::this_coro::cancellation_state;
    if (is_cancelled(cancellation)) {
      co_return std::unexpected(core::Error::cancelled());
    }
    co_await asio::post(ctx.executor, asio::use_awaitable);
    if (is_cancelled(cancellation)) {
      co_return std::unexpected(core::Error::cancelled());
    }
    entries = list_directory_recursive(*parsed, cancellation);
  } else {
    io::ListDirectoryOptions options{
        .include_hidden = parsed->include_hidden,
        .max_entries = parsed->max_entries,
    };
    entries = co_await io::list_directory(ctx.executor, std::move(parsed->path), options);
    if (entries) {
      apply_display_paths(*parsed, *entries);
    }
  }
  if (!entries) {
    co_return std::unexpected(std::move(entries).error());
  }

  auto text = render(*entries);
  const auto display_root =
      parsed->workspace == nullptr ? resolved_path : parsed->workspace->display_path(resolved_path);
  auto data_json = format_data_json(display_root, *parsed, *entries);
  const auto match_count = static_cast<std::uint64_t>(entries->size());
  const auto files_touched = parsed->recursive ? recursive_files_touched(entries->size()) : std::uint32_t{1};
  co_return Output{
      .text = std::move(text),
      .data_json = std::move(data_json),
      .usage =
          ToolUsage{
              .files_touched = files_touched,
              .match_count = match_count,
          },
  };
}

}  // namespace

core::Result<void> register_directory_list(Registry& registry) {
  core::ToolDef def{
      .name = std::string{kDirectoryListName},
      .description = "List the immediate children of a directory. Input: {\"path\": <string>, "
                     "\"include_hidden\"?: bool (default false), \"recursive\"?: bool (default false), "
                     "\"max_entries\"?: positive integer (default 256)}. With `recursive=true`, walks the whole "
                     "tree while skipping nested symlinks plus `.git`, `.xmake`, `.orangutan`, `build`, and "
                     "`node_modules` directories and honoring `.gitignore` / `.ignore` files from the listing root "
                     "downward. Returns one `<path>:<kind>:<size_bytes or '-'>` line per entry, "
                     "sorted by path; `no entries` when the directory is empty. `kind` is one of "
                     "`regular_file` | `directory` | `symlink` | `other`. Errors with `io` if the "
                     "directory has more entries than `max_entries`; raise the cap and retry. "
                     "Successful calls also fill `data_json` with kind, path, include_hidden, recursive, "
                     "max_entries, entry_count, and an `entries[]` array of "
                     "`{name, path, kind, size_bytes}` (size_bytes is null for non-regular files); "
                     "`usage` reports `files_touched=1` for single-level listings or root-plus-entry count for "
                     "recursive listings, and `match_count` (entry count). When a Workspace is supplied, output "
                     "paths use stable display labels such as `<workspace>/src/main.cpp` instead of raw absolute "
                     "paths.",
      .input_schema_json = std::string{kDirectoryListSchema},
      .required_capabilities = {core::Capability::list_directory},
      .deferred = false,
      .category = "directory",
  };
  return registry.add(std::move(def), &directory_list_handler);
}

}  // namespace orangutan::tool
