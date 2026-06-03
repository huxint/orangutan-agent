// src/oran-tool/directory_list.cpp — `directory.list` built-in.
//
// Thin formatter on top of `oran-io::list_directory`: parses the JSON input,
// validates the typed options, dispatches the existing coroutine helper, and
// renders one `path:kind:size_bytes` line per entry. The helper already
// sorts by path and applies the hidden / max-entries filters, so the tool
// adds no scanning logic of its own — the perf surface is identical to the
// `bench/io` coverage of `list_directory`. Slice 29 ships the registrar
// alongside the slice-17 / 18 / 19 / 20 file-tool catalog. Slice 40 routes
// the input path through `tool::Workspace::resolve_list` when
// `DispatchContext::workspace` is supplied so the listing cannot escape the
// workspace via traversal or a root-side symlink; the underlying
// `oran-io::list_directory` semantics are unchanged.
//
// Slice 64 (2026-05-24) closes spec 0014's third built-in migration step
// (after slice 62 `file.read` and slice 63 `file.search`): successful calls
// keep the existing `<path>:<kind>:<size_bytes or '-'>` text rendering and
// also fill `Output::data_json` with `{kind:"directory_list", path,
// include_hidden, max_entries, entry_count, entries[]}`, where each entry
// carries `{name, path, kind, size_bytes}`. `Output::usage` is filled with
// `files_touched=1` (the directory itself) and `match_count` (entry
// count) so audit fan-out and the future scheduler can see directory-walk
// cost without parsing prose.

#include <oran/tool/builtins.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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
    R"("max_entries":{"type":"integer","minimum":1}},"required":["path"],"additionalProperties":false})";

constexpr std::size_t kDirectoryListDefaultMax = 256;

struct ParsedInput {
  std::string path;
  bool include_hidden{false};
  std::size_t max_entries{kDirectoryListDefaultMax};
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
      return std::unexpected(core::Error::invalid_argument("directory.list: `include_hidden` must be a boolean"));
    }
    result.include_hidden = (*parsed)["include_hidden"].get<bool>();
  }

  if (parsed->contains("max_entries")) {
    if (!(*parsed)["max_entries"].is_number_unsigned()) {
      return std::unexpected(core::Error::invalid_argument("directory.list: `max_entries` must be a positive integer"));
    }
    const auto raw = (*parsed)["max_entries"].get<std::uint64_t>();
    if (raw == 0) {
      return std::unexpected(core::Error::invalid_argument("directory.list: `max_entries` must be greater than zero"));
    }
    result.max_entries = static_cast<std::size_t>(raw);
  }

  return result;
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

  if (ctx.resolved_path.has_value()) {
    parsed->path = ctx.resolved_path->absolute_path;
  } else if (ctx.workspace != nullptr) {
    auto resolved = ctx.workspace->resolve_list(parsed->path);
    if (!resolved) {
      co_return std::unexpected(std::move(resolved).error());
    }
    parsed->path = std::move(resolved->absolute_path);
  }

  io::ListDirectoryOptions options{
      .include_hidden = parsed->include_hidden,
      .max_entries = parsed->max_entries,
  };
  const auto resolved_path = parsed->path;
  auto entries = co_await io::list_directory(ctx.executor, std::move(parsed->path), options);
  if (!entries) {
    co_return std::unexpected(std::move(entries).error());
  }

  auto text = render(*entries);
  auto data_json = format_data_json(resolved_path, *parsed, *entries);
  const auto match_count = static_cast<std::uint64_t>(entries->size());
  co_return Output{
      .text = std::move(text),
      .data_json = std::move(data_json),
      .usage =
          ToolUsage{
              .files_touched = 1,
              .match_count = match_count,
          },
  };
}

}  // namespace

core::Result<void> register_directory_list(Registry& registry) {
  core::ToolDef def{
      .name = std::string{kDirectoryListName},
      .description = "List the immediate children of a directory. Input: {\"path\": <string>, "
                     "\"include_hidden\"?: bool (default false), \"max_entries\"?: positive integer (default 256)}. "
                     "Returns one `<path>:<kind>:<size_bytes or '-'>` line per entry, sorted by path; "
                     "`no entries` when the directory is empty. `kind` is one of "
                     "`regular_file` | `directory` | `symlink` | `other`. Errors with `io` if the "
                     "directory has more entries than `max_entries`; raise the cap and retry. "
                     "Successful calls also fill `data_json` with kind, path, include_hidden, "
                     "max_entries, entry_count, and an `entries[]` array of "
                     "`{name, path, kind, size_bytes}` (size_bytes is null for non-regular files); "
                     "`usage` reports `files_touched=1` (the directory) and `match_count` (entry count).",
      .input_schema_json = std::string{kDirectoryListSchema},
      .required_capabilities = {core::Capability::list_directory},
      .deferred = false,
      .category = "directory",
  };
  return registry.add(std::move(def), &directory_list_handler);
}

}  // namespace orangutan::tool
