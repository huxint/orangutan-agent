// include/oran/tool/builtins.hpp — built-in tool registrars.
//
// Each `register_*` free function adds one tool to a `tool::Registry`,
// returning the same `Result<void>` shape `Registry::add` does so callers
// can early-return on the first failure. The aggregate `register_builtins`
// wires every tool this slice ships in catalog order.
//
// Slice 29 grows the catalog to `file.read` + `file.write` + `file.edit` +
// `file.search` + `directory.list`; future slices will fold in the shell
// tools and so on. This header is the one place callers learn what shipped.

#pragma once

#include <string_view>

#include <oran/core/result.hpp>
#include <oran/tool/registry.hpp>

namespace orangutan::tool {

/// Stable wire name for the file-read built-in.
inline constexpr std::string_view kFileReadName{"file.read"};

/// Stable wire name for the file-write built-in.
inline constexpr std::string_view kFileWriteName{"file.write"};

/// Stable wire name for the file-edit built-in.
inline constexpr std::string_view kFileEditName{"file.edit"};

/// Stable wire name for the file-search built-in.
inline constexpr std::string_view kFileSearchName{"file.search"};

/// Stable wire name for the directory-list built-in.
inline constexpr std::string_view kDirectoryListName{"directory.list"};

/// Register the `file.read` tool. Reads UTF-8 content from a workspace path
/// using `oran-io`'s coroutine helper; capability `read_file` is required.
[[nodiscard]] core::Result<void> register_file_read(Registry& registry);

/// Register the `file.write` tool. Writes UTF-8 content to a path using
/// `oran-io`'s coroutine helper; capability `write_file` is required.
/// Input shape: `{"path": <string>, "content": <string>, "mode"?:
/// "truncate"|"append"|"fail_if_exists", "create_parents"?: bool}`.
[[nodiscard]] core::Result<void> register_file_write(Registry& registry);

/// Register the `file.edit` tool. Replaces `old_string` with `new_string` in
/// a UTF-8 text file; capability `edit_file` is required. Input shape:
/// `{"path": <string>, "old_string": <string>, "new_string": <string>,
/// "replace_all"?: bool}`. Returns `conflict` if `old_string` is not unique
/// unless `replace_all` is set; `not_found` if `old_string` does not occur.
[[nodiscard]] core::Result<void> register_file_edit(Registry& registry);

/// Register the `file.search` tool. Scans a UTF-8 text file or (recursively)
/// a directory for literal substring matches; capability `read_file` is
/// required. Input shape: `{"path": <string>, "pattern": <string>,
/// "max_matches"?: uint (default 100), "include_hidden"?: bool (default
/// false)}`. Returns one `path:line:text` line per match, with a trailing
/// `(truncated; matches capped at <N>)` summary when the cap is hit;
/// returns the literal text `no matches` (non-error) when no match was
/// found. Files containing NUL bytes in their first 8 KiB are treated as
/// binary and skipped during a directory walk.
[[nodiscard]] core::Result<void> register_file_search(Registry& registry);

/// Register the `directory.list` tool. Enumerates the immediate children of
/// a directory through `oran-io::list_directory`; capability
/// `list_directory` is required. Input shape:
/// `{"path": <string>, "include_hidden"?: bool (default false),
/// "max_entries"?: uint (default 256)}`. Returns one
/// `<path>:<kind>:<size_bytes or '-'>` line per entry, sorted by path
/// (the order `oran-io::list_directory` already enforces); the literal
/// text `no entries` (non-error) when the directory is empty after the
/// hidden filter. `kind` is the `io::DirectoryEntryKind` wire spelling
/// (`regular_file` | `directory` | `symlink` | `other`); `size_bytes`
/// is a decimal integer for regular files and the literal `-` for every
/// other kind. The call returns an `io` error when the directory has
/// strictly more than `max_entries` entries — raise the cap and retry.
[[nodiscard]] core::Result<void> register_directory_list(Registry& registry);

/// Register every built-in this slice ships. Currently wires `file.read`,
/// `file.write`, `file.edit`, `file.search`, then `directory.list`; future
/// slices append additional tools so production callers can stay on this
/// single entry point.
[[nodiscard]] core::Result<void> register_builtins(Registry& registry);

}  // namespace orangutan::tool
