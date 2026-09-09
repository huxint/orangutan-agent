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

/// Stable wire name for the long-term memory recall built-in.
inline constexpr std::string_view kMemoryRecallName{"MemoryRecall"};

/// Stable wire name for the long-term memory write built-in.
inline constexpr std::string_view kMemoryRememberName{"MemoryRemember"};

/// Stable wire name for the long-term memory delete built-in.
inline constexpr std::string_view kMemoryForgetName{"MemoryForget"};

inline constexpr std::string_view AGENT_RUN_NAME{"AgentRun"};

/// Register a child-run tool limited to the configured names. The host supplies
/// the runner on DispatchContext; each call requires spawn_agent authority.
[[nodiscard]] core::Result<void> register_agent_run(Registry& registry, std::span<const std::string> agent_names);

/// Register the `FileRead` tool. Reads UTF-8 content using `oran-io`'s
/// coroutine helper; when `DispatchContext::workspace` is set, the input path
/// is first resolved through `tool::Workspace`. Capability `read_file` is
/// required. Input shape: `{"path": <string>, "start_line"?, "line_count"?,
/// "offset_bytes"?, "length_bytes"?, "max_bytes"? (<= 16 MiB), "if_version"?,
/// "allow_outside_workspace"?: bool}`. `allow_outside_workspace=true` is a
/// one-off read/list escape that must be approved at dispatch time.
/// Line and byte ranges are mutually exclusive. Output is a header line
/// `<path>:<start>-<end> fingerprint=<token> bytes=<n>[ truncated]` followed
/// by the requested file slice on the next line. `if_version` matching the
/// current fingerprint short-circuits to `Error::not_modified`. Successful
/// reads also fill `Output::data_json` with the requested text plus
/// range/fingerprint metadata, and fill `Output::usage.bytes_read`,
/// `files_touched`, and `truncated`.
[[nodiscard]] core::Result<void> register_file_read(Registry& registry);

/// Register the `FileWrite` tool. Writes UTF-8 content through the resolved
/// workspace authority; capability `write_file` is required and direct
/// dispatch callers must provide `DispatchContext::workspace`.
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
/// a UTF-8 text file through the resolved workspace authority; capability
/// `edit_file` is required and direct dispatch callers must provide
/// `DispatchContext::workspace`. Input shape:
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

/// Register the three filesystem tools.
[[nodiscard]] core::Result<void> register_builtins(Registry& registry);

/// Register the memory tools after the host has supplied memory services.
[[nodiscard]] core::Result<void> register_memory_tools(Registry& registry);

}  // namespace orangutan::tool
