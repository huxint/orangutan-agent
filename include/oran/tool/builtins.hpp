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
inline constexpr std::string_view kFileReadName{"file.read"};

/// Stable wire name for the file-write built-in.
inline constexpr std::string_view kFileWriteName{"file.write"};

/// Stable wire name for the file-edit built-in.
inline constexpr std::string_view kFileEditName{"file.edit"};

/// Stable wire name for the file-search built-in.
inline constexpr std::string_view kFileSearchName{"file.search"};

/// Stable wire name for the directory-list built-in.
inline constexpr std::string_view kDirectoryListName{"directory.list"};

/// Stable wire name for the file-delete built-in.
inline constexpr std::string_view kFileDeleteName{"file.delete"};

/// Stable wire name for the catalog metadata lookup built-in.
inline constexpr std::string_view kToolSearchName{"tool.search"};

/// Stable wire name for the markdown skill invocation built-in.
inline constexpr std::string_view kSkillInvokeName{"skill.invoke"};

/// Stable wire name for the markdown skill deactivation built-in.
inline constexpr std::string_view kSkillDeactivateName{"skill.deactivate"};

/// Stable wire name for the long-term memory recall built-in.
inline constexpr std::string_view kMemoryRecallName{"memory.recall"};

/// Stable wire name for the long-term memory write built-in.
inline constexpr std::string_view kMemoryRememberName{"memory.remember"};

/// Stable wire name for the long-term memory delete built-in.
inline constexpr std::string_view kMemoryForgetName{"memory.forget"};

/// Register the `file.read` tool. Reads UTF-8 content using `oran-io`'s
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

/// Register the `file.write` tool. Writes UTF-8 content to a path using
/// `oran-io`'s coroutine helper; capability `write_file` is required.
/// Input shape: `{"path": <string>, "content": <string>, "mode"?:
/// "truncate"|"append"|"fail_if_exists", "create_parents"?: bool,
/// "max_bytes"?: positive integer <= 16777216,
/// "expected_version"?: <version token from a prior `file.read`>}`. When
/// `expected_version` is supplied the call fails with `conflict`
/// (reason=stale_fingerprint, current `fingerprint` in context) if the
/// file's current version differs. Successful writes fill
/// `Output::usage.bytes_written` and `files_touched`.
[[nodiscard]] core::Result<void> register_file_write(Registry& registry);

/// Register the `file.edit` tool. Replaces `old_string` with `new_string` in
/// a UTF-8 text file; capability `edit_file` is required. Input shape:
/// `{"path": <string>, "old_string": <string>, "new_string": <string>,
/// "replace_all"?: bool, "max_bytes"?: positive integer <= 16777216,
/// "expected_version"?: <version token from a prior `file.read`>}`.
/// Returns `conflict` if `old_string` is not unique unless `replace_all`
/// is set; `not_found` if `old_string` does not occur. When
/// `expected_version` is supplied the call fails with `conflict`
/// (reason=stale_fingerprint, current `fingerprint` in context) if the
/// file's current version differs. Successful edits fill
/// `Output::usage.bytes_read`, `bytes_written`, `files_touched`, and
/// `match_count`.
[[nodiscard]] core::Result<void> register_file_edit(Registry& registry);

/// Register the `file.search` tool. Scans a UTF-8 text file or (recursively)
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
/// (post-truncation), and the `truncated` cap flag.
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
/// Successful calls also fill `Output::data_json` with `kind`, `path`,
/// `include_hidden`, `max_entries`, `entry_count`, and an `entries[]`
/// array of `{name, path, kind, size_bytes}` (size_bytes is JSON null
/// for non-regular files), and fill `Output::usage` with
/// `files_touched=1` (the directory itself) and `match_count`
/// (the entry count).
[[nodiscard]] core::Result<void> register_directory_list(Registry& registry);

/// Register the `file.delete` tool. Deletes the regular file at `path`
/// through `oran-io::delete_file`; capability `delete_path` is required.
/// Input shape: `{"path": <string>}`. Returns `invalid_argument` when the
/// path is a directory or a symlink (the v1 surface refuses anything but
/// a regular file so an LLM-driven delete cannot recursively destroy a
/// tree or unlink a symlink to a directory outside the workspace);
/// `not_found` when no file exists at `path`. Successful deletes return
/// the literal text `deleted <path>` and fill
/// `Output::usage.bytes_written=0` plus `files_touched=1`.
[[nodiscard]] core::Result<void> register_file_delete(Registry& registry);

/// Register the `tool.search` tool. Searches the current registry catalog by
/// exact `name`, exact `category`, and/or declared `capability`; at least one
/// selector is required and supplied selectors are ANDed. Input shape:
/// `{"name"?: <string>, "category"?: <string>, "capability"?:
/// <Capability wire name>}`. Metadata lookup requires no runtime capability,
/// is not deferred, and returns a text fallback plus structured `data_json`
/// with `kind`, `query`, `match_count`, and `matches[]` entries carrying the
/// matched tool's name, description, input schema, required capabilities,
/// deferred flag, and category.
[[nodiscard]] core::Result<void> register_tool_search(Registry& registry);

/// Register the `skill.invoke` tool. Runs a markdown skill from the runtime's
/// current skill snapshot; capability `invoke_skill` is required. Input shape:
/// `{"name": <string>, "inputs"?: <JSON value>}`. The concrete skill lookup
/// lives behind `DispatchContext::skill_invoke`, so this built-in remains an
/// ordinary permissioned/audited registry dispatch without making `oran-tool`
/// depend on `oran-skill`.
[[nodiscard]] core::Result<void> register_skill_invoke(Registry& registry);

/// Register the `skill.deactivate` tool. Clears a loaded skill's active marker
/// from the next prompt's section 4 catalog; capability `deactivate_skill` is
/// required. Input shape: `{"name": <string>}`. The concrete skill lookup lives
/// behind `DispatchContext::skill_deactivate`, so this built-in stays an
/// ordinary permissioned/audited registry dispatch without making `oran-tool`
/// depend on `oran-skill`. A successful deactivation returns a short
/// confirmation text plus a versioned `skill_deactivation` record in
/// `Output::data_json`; the next prompt boundary nets it against any prior
/// `skill.invoke` activation so the skill stops appearing as active.
[[nodiscard]] core::Result<void> register_skill_deactivate(Registry& registry);

/// Register the `memory.recall` tool. Searches long-term memory through the
/// runtime supplied on `DispatchContext::memory_recall`; capability
/// `read_memory` is required. Input shape: `{"query": <string>,
/// "limit"?: positive integer <= 20 (default 5), "kinds"?: [<RecordKind wire
/// spelling>]}`. The concrete memory runtime lives outside `oran-tool`, so
/// this built-in remains an ordinary permissioned/audited registry dispatch
/// without making the tool library depend on `oran-memory`. Successful calls
/// return deterministic recall text plus structured `data_json` with recalled
/// record metadata.
[[nodiscard]] core::Result<void> register_memory_recall(Registry& registry);

/// Register the `memory.remember` tool. Upserts one long-term memory record
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

/// Register the `memory.forget` tool. Removes one long-term memory record in
/// the current agent scope through the runtime supplied on
/// `DispatchContext::memory_forget`; capability `write_memory` is required.
/// Input shape: `{"id": <string>}`. The concrete memory backend lives outside
/// `oran-tool`, so this built-in remains an ordinary permissioned/audited
/// registry dispatch without making the tool library depend on `oran-memory`.
/// Successful calls are idempotent and return a short confirmation text plus
/// structured `data_json` with the scoped removed key.
[[nodiscard]] core::Result<void> register_memory_forget(Registry& registry);

/// Register every built-in this repository ships. Currently wires `file.read`,
/// `file.write`, `file.edit`, `file.search`, `directory.list`, then
/// `file.delete`, then `tool.search`, then `skill.invoke`, then
/// `skill.deactivate`, then `memory.recall`, then `memory.remember`, then
/// `memory.forget`; future slices append additional tools so production callers
/// can stay on this single entry point.
[[nodiscard]] core::Result<void> register_builtins(Registry& registry);

}  // namespace orangutan::tool
