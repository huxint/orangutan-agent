// src/oran-tool/directory_list.cpp — `directory.list` built-in.
//
// Thin formatter on top of `oran-io::list_directory`: parses the JSON input,
// validates the typed options, dispatches the existing coroutine helper, and
// renders one `path:kind:size_bytes` line per entry. The helper already
// sorts by path and applies the hidden / max-entries filters, so the tool
// adds no scanning logic of its own — the perf surface is identical to the
// `bench/io` coverage of `list_directory`. Slice 29 ships the registrar
// alongside the slice-17 / 18 / 19 / 20 file-tool catalog.

#include <oran/tool/builtins.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
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
  nlohmann::json parsed;
  try {
    parsed = nlohmann::json::parse(input_json);
  } catch (const nlohmann::json::parse_error& e) {
    return std::unexpected(
        core::Error::invalid_argument("directory.list: input is not valid JSON").with("detail", e.what()));
  } catch (const std::exception& e) {
    return std::unexpected(
        core::Error::invalid_argument("directory.list: input is not valid JSON").with("detail", e.what()));
  }

  if (!parsed.is_object() || !parsed.contains("path") || !parsed["path"].is_string()) {
    return std::unexpected(
        core::Error::invalid_argument("directory.list: input must be an object with a string `path` field"));
  }

  ParsedInput result;
  result.path = parsed["path"].get<std::string>();

  if (parsed.contains("include_hidden")) {
    if (!parsed["include_hidden"].is_boolean()) {
      return std::unexpected(core::Error::invalid_argument("directory.list: `include_hidden` must be a boolean"));
    }
    result.include_hidden = parsed["include_hidden"].get<bool>();
  }

  if (parsed.contains("max_entries")) {
    if (!parsed["max_entries"].is_number_unsigned()) {
      return std::unexpected(core::Error::invalid_argument("directory.list: `max_entries` must be a positive integer"));
    }
    const auto raw = parsed["max_entries"].get<std::uint64_t>();
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
    out.append(entry.path);
    out.push_back(':');
    out.append(core::enum_name(entry.kind));
    out.push_back(':');
    if (entry.size_bytes.has_value()) {
      out.append(std::to_string(*entry.size_bytes));
    } else {
      out.push_back('-');
    }
  }
  return out;
}

[[nodiscard]] async::Awaitable<core::Result<Output>> directory_list_handler(std::string_view input_json,
                                                                            DispatchContext& ctx) {
  auto parsed = parse_input(input_json);
  if (!parsed) {
    co_return std::unexpected(std::move(parsed).error());
  }

  io::ListDirectoryOptions options{
      .include_hidden = parsed->include_hidden,
      .max_entries = parsed->max_entries,
  };
  auto entries = co_await io::list_directory(ctx.executor, std::move(parsed->path), options);
  if (!entries) {
    co_return std::unexpected(std::move(entries).error());
  }

  co_return Output{.text = render(*entries)};
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
                     "directory has more entries than `max_entries`; raise the cap and retry.",
      .input_schema_json = std::string{kDirectoryListSchema},
      .required_capabilities = {core::Capability::list_directory},
  };
  return registry.add(std::move(def), &directory_list_handler);
}

}  // namespace orangutan::tool
