// include/oran/tool/builtins.hpp — built-in tool registrars.
//
// Each `register_*` free function adds one tool to a `tool::Registry`,
// returning the same `Result<void>` shape `Registry::add` does so callers
// can early-return on the first failure. The aggregate `register_builtins`
// wires every tool this slice ships in catalog order.
//
// Slice 19 grows the catalog to `file.read` + `file.write` + `file.edit`;
// future slices will fold in `file.search`, the shell tools, and so on. This
// header is the one place callers learn what shipped.

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

/// Register every built-in this slice ships. Currently wires `file.read`,
/// `file.write`, then `file.edit`; future slices append additional tools so
/// production callers can stay on this single entry point.
[[nodiscard]] core::Result<void> register_builtins(Registry& registry);

}  // namespace orangutan::tool
