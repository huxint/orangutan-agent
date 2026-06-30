// include/oran/tool/builtins.hpp — built-in tool registrars.
//
// Each `register_*` free function adds one tool to a `tool::Registry`,
// returning the same `Result<void>` shape `Registry::add` does so callers
// can early-return on the first failure. The aggregate `register_builtins`
// wires every tool this repository currently ships in catalog order.
//
// This header is the one place callers learn what shipped.

#pragma once

#include <string_view>

#include <oran/core/result.hpp>
#include <oran/tool/registry.hpp>

namespace orangutan::tool {

/// Stable wire name for the file-read built-in.
inline constexpr std::string_view kFileReadName{"FileRead"};

/// Stable wire name for the file-write built-in.
inline constexpr std::string_view kFileWriteName{"FileWrite"};

/// Stable wire name for the file-edit built-in.
inline constexpr std::string_view kFileEditName{"FileEdit"};

/// Stable wire name for the file-search built-in.
inline constexpr std::string_view kFileSearchName{"FileSearch"};

/// Stable wire name for the directory-list built-in.
inline constexpr std::string_view kDirectoryListName{"DirectoryList"};

/// Stable wire name for the file-delete built-in.
inline constexpr std::string_view kFileDeleteName{"FileDelete"};

/// Stable wire name for the catalog metadata lookup built-in.
inline constexpr std::string_view kToolSearchName{"ToolSearch"};

/// Stable wire name for the markdown skill invocation built-in.
inline constexpr std::string_view kSkillInvokeName{"SkillInvoke"};

/// Stable wire name for the markdown skill deactivation built-in.
inline constexpr std::string_view kSkillDeactivateName{"SkillDeactivate"};

/// Stable wire name for the long-term memory recall built-in.
inline constexpr std::string_view kMemoryRecallName{"MemoryRecall"};

/// Stable wire name for the long-term memory write built-in.
inline constexpr std::string_view kMemoryRememberName{"MemoryRemember"};

/// Stable wire name for the long-term memory delete built-in.
inline constexpr std::string_view kMemoryForgetName{"MemoryForget"};

/// Register the `FileRead` tool. Reads UTF-8 content using `oran-io`'s
/// coroutine helper; when `DispatchContext::workspace` is set, the input path
/// is first resolved through `tool::Workspace`. Capability `read_file` is
/// required. Input shape: `{"path": <string>, "start_line"?, "line_count"?,
/// "offset_bytes"?, "length_bytes"?, "max_bytes"? (<= 16 MiB), "if_version"?}`.
/// Line and byte ranges are mutually exclusive. Output is a header line
/// `<path>:<start>-<end> fingerprint=<token> bytes=<n>[ truncated]` followed
/// by the requested file slice on the next line. `if_version` matching the
/// current fingerprint short-circuits to `Error::not_modified`. Successful
/// reads also fill `Output::data_json` with the requested text plus
/// range/fingerprint metadata, and fill `Output::usage.bytes_read`,
/// `files_touched`, and `truncated`.
[[nodiscard]] core::Result<void> register_file_read(Registry& registry);

/// Register the `FileWrite` tool. Writes UTF-8 content to a path using
/// `oran-io`'s coroutine helper; capability `write_file` is required.
/// Input shape: `{"path": <string>, "content": <string>, "mode"?:
/// "truncate"|"append"|"fail_if_exists", "create_parents"?: bool,
/// "max_bytes"?: positive integer <= 16777216,
/// "expected_version"?: <version token from a prior `FileRead`>}`. When
/// `expected_version` is supplied the call fails with `conflict`
/// (reason=stale_fingerprint, current `fingerprint` in context) if the
/// file's current version differs. Successful writes fill
/// `Output::usage.bytes_written` and `files_touched`.
[[nodiscard]] core::Result<void> register_file_write(Registry& registry);

/// Register the `FileEdit` tool. Replaces `old_string` with `new_string` in
/// a UTF-8 text file; capability `edit_file` is required. Input shape:
/// `{"path": <string>, "old_string": <string>, "new_string": <string>,
/// "replace_all"?: bool, "max_bytes"?: positive integer <= 16777216,
/// "expected_version"?: <version token from a prior `FileRead`>}`.
/// Returns `conflict` if `old_string` is not unique unless `replace_all`
/// is set; `not_found` if `old_string` does not occur. When
/// `expected_version` is supplied the call fails with `conflict`
/// (reason=stale_fingerprint, current `fingerprint` in context) if the
/// file's current version differs. Successful edits fill
/// `Output::usage.bytes_read`, `bytes_written`, `files_touched`, and
/// `match_count`.
[[nodiscard]] core::Result<void> register_file_edit(Registry& registry);

/// Register the `FileSearch` tool. Scans a UTF-8 text file or (recursively)
/// a directory for literal substring matches; capability `read_file` is
/// required. Input shape: `{"path": <string>, "pattern": <string>,
/// "max_matches"?: uint (default 100), "include_hidden"?: bool (default
/// false), "regex"?: bool (default false), "max_output_bytes"?: uint
/// (default 1048576), "respect_ignore"?: bool (default true)}`.
/// `regex=true` compiles through `permission::InputPattern` and reuses a
/// bounded process-local compiled-pattern cache across dispatches.
/// Returns one
/// `path:line:text` line per match, with a trailing `(truncated; matches
/// capped at <N>)` or `(truncated; output capped at <N> bytes)` summary
/// when a cap is hit; returns the literal text `no matches` (non-error)
/// when no match was found. Files containing NUL bytes in their first 8
/// KiB are treated as binary and skipped during a directory walk. When
/// `respect_ignore=true` (the default), the recursive walk skips `.git`,
/// `.xmake`, `.orangutan`, `build`, and `node_modules` directories
/// regardless of `include_hidden`, and honours `.gitignore` / `.ignore`
/// files from the search root downward for comments, blanks, escaped
/// leading `#` / `!` literals, `!` negation, trailing `/` directory rules,
/// slash-relative patterns, basename patterns, and fnmatch-style globs.
/// Successful calls also fill `Output::data_json` with `kind`, `path`,
/// `pattern`, `regex`, `matches[]`, `match_count`, `truncated`,
/// `truncation_reason`, `files_scanned`, and `bytes_read`, and fill
/// `Output::usage` with `bytes_read` (cumulative scanned file bytes),
/// `files_touched` (non-binary scanned file count), `match_count`
/// (post-truncation), and the `truncated` cap flag. When a `Workspace` is
/// supplied, output paths use stable display labels such as
/// `<workspace>/src/main.cpp` instead of raw absolute paths.
[[nodiscard]] core::Result<void> register_file_search(Registry& registry);

/// Register the `DirectoryList` tool. Enumerates the immediate children of
/// a directory through `oran-io::list_directory` by default, or walks the
/// whole tree when `recursive=true`; capability `list_directory` is
/// required. Input shape:
/// `{"path": <string>, "include_hidden"?: bool (default false),
/// "recursive"?: bool (default false), "max_entries"?: uint (default
/// 256)}`. Recursive listings skip nested symlinks plus `.git`, `.xmake`,
/// `.orangutan`, `build`, and `node_modules` directories, and honour
/// `.gitignore` / `.ignore` files from the listing root downward. Returns one
/// `<path>:<kind>:<size_bytes or '-'>` line per entry, sorted by path;
/// the literal text `no entries` (non-error) when the directory is empty
/// after the filters. `kind` is the `io::DirectoryEntryKind` wire spelling
/// (`regular_file` | `directory` | `symlink` | `other`); `size_bytes` is a
/// decimal integer for regular files and the literal `-` for every other
/// kind. The call returns an `io` error when the directory has strictly
/// more than `max_entries` entries — raise the cap and retry. Successful
/// calls also fill `Output::data_json` with `kind`, `path`,
/// `include_hidden`, `recursive`, `max_entries`, `entry_count`, and an
/// `entries[]` array of `{name, path, kind, size_bytes}` (size_bytes is
/// JSON null for non-regular files), and fill `Output::usage` with
/// `files_touched=1` for single-level listings or root-plus-entry count
/// for recursive listings, and `match_count` (the entry count). When a
/// `Workspace` is supplied, output paths use stable display labels such as
/// `<workspace>/src/main.cpp` instead of raw absolute paths.
[[nodiscard]] core::Result<void> register_directory_list(Registry& registry);

/// Register the `FileDelete` tool. Deletes a regular file, or a directory
/// tree when `recursive=true`, through `oran-io::delete_path`; capability
/// `delete_path` is required. Input shape:
/// `{"path": <string>, "recursive"?: bool}`. Returns `invalid_argument`
/// when the path is a directory and recursion intent is absent, or when the
/// path is a symlink; `not_found` when no path exists. Successful deletes
/// return the literal text `deleted <path>` and fill
/// `Output::usage.bytes_written=0` plus `files_touched` equal to the
/// removed path count.
[[nodiscard]] core::Result<void> register_file_delete(Registry& registry);

/// Register the `ToolSearch` tool. Searches the current registry catalog by
/// exact `name`, exact `category`, and/or declared `capability`; at least one
/// selector is required and supplied selectors are ANDed. Input shape:
/// `{"name"?: <string>, "category"?: <string>, "capability"?:
/// <Capability wire name>}`. Metadata lookup requires no runtime capability,
/// is not deferred, and returns a text fallback plus structured `data_json`
/// with `kind`, `query`, `match_count`, and `matches[]` entries carrying the
/// matched tool's name, description, input schema, required capabilities,
/// deferred flag, and category.
[[nodiscard]] core::Result<void> register_tool_search(Registry& registry);

/// Register the `SkillInvoke` tool. Runs a markdown skill from the runtime's
/// current skill snapshot; capability `invoke_skill` is required. Input shape:
/// `{"name": <string>, "inputs"?: <JSON value>}`. The concrete skill lookup
/// lives behind `DispatchContext::skill_invoke`, so this built-in remains an
/// ordinary permissioned/audited registry dispatch without making `oran-tool`
/// depend on `oran-skill`.
[[nodiscard]] core::Result<void> register_skill_invoke(Registry& registry);

/// Register the `SkillDeactivate` tool. Clears a loaded skill's active marker
/// from the next prompt's section 4 catalog; capability `deactivate_skill` is
/// required. Input shape: `{"name": <string>}`. The concrete skill lookup lives
/// behind `DispatchContext::skill_deactivate`, so this built-in stays an
/// ordinary permissioned/audited registry dispatch without making `oran-tool`
/// depend on `oran-skill`. A successful deactivation returns a short
/// confirmation text plus a versioned `skill_deactivation` record in
/// `Output::data_json`; the next prompt boundary nets it against any prior
/// `SkillInvoke` activation so the skill stops appearing as active.
[[nodiscard]] core::Result<void> register_skill_deactivate(Registry& registry);

/// Register the `MemoryRecall` tool. Searches long-term memory through the
/// runtime supplied on `DispatchContext::memory_recall`; capability
/// `read_memory` is required. Input shape: `{"query": <string>,
/// "limit"?: positive integer <= 20 (default 5), "kinds"?: [<RecordKind wire
/// spelling>]}`. The concrete memory runtime lives outside `oran-tool`, so
/// this built-in remains an ordinary permissioned/audited registry dispatch
/// without making the tool library depend on `oran-memory`. Successful calls
/// return deterministic recall text plus structured `data_json` with recalled
/// record metadata.
[[nodiscard]] core::Result<void> register_memory_recall(Registry& registry);

/// Register the `MemoryRemember` tool. Upserts one long-term memory record
/// through the runtime supplied on `DispatchContext::memory_remember`;
/// capability `write_memory` is required. Input shape:
/// `{"id": <string>, "kind": <RecordKind wire spelling>, "title": <string>,
/// "body": <string>, "importance"?: number in [0,1] (default 0.5),
/// "tags"?: [<string>], "linked_record_ids"?: [<string>], "shadow"?: bool}`.
/// The concrete memory backend lives outside `oran-tool`, so this built-in
/// remains an ordinary permissioned/audited registry dispatch without making
/// the tool library depend on `oran-memory`. Successful calls return a short
/// confirmation text plus structured `data_json` with saved record metadata.
[[nodiscard]] core::Result<void> register_memory_remember(Registry& registry);

/// Register the `MemoryForget` tool. Removes one long-term memory record in
/// the current agent scope through the runtime supplied on
/// `DispatchContext::memory_forget`; capability `write_memory` is required.
/// Input shape: `{"id": <string>}`. The concrete memory backend lives outside
/// `oran-tool`, so this built-in remains an ordinary permissioned/audited
/// registry dispatch without making the tool library depend on `oran-memory`.
/// Successful calls are idempotent and return a short confirmation text plus
/// structured `data_json` with the scoped removed key.
[[nodiscard]] core::Result<void> register_memory_forget(Registry& registry);

/// Register every built-in this repository ships. Currently wires `FileRead`,
/// `FileWrite`, `FileEdit`, `FileSearch`, `DirectoryList`, then
/// `FileDelete`, then `ToolSearch`, then `SkillInvoke`, then
/// `SkillDeactivate`, then `MemoryRecall`, then `MemoryRemember`, then
/// `MemoryForget`; future slices append additional tools so production callers
/// can stay on this single entry point.
[[nodiscard]] core::Result<void> register_builtins(Registry& registry);

}  // namespace orangutan::tool
