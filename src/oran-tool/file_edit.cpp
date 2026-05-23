// src/oran-tool/file_edit.cpp — `file.edit` built-in.
//
// Slice 19 of the v2 stack. Composes `io::read_text_file` and
// `io::write_text_file` with an in-process substring replacement so a small
// MVP can ship without pulling a patch parser into the build. The
// design doc's "patch-style edits with conflict detection" still holds as the
// long-term shape — see `Design Intent` in the slice history for why this
// slice ships the simpler `old_string` / `new_string` surface that Claude
// Code's own Edit tool exposes.

#include <oran/tool/builtins.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/capability.hpp>
#include <oran/core/error.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/io/file.hpp>
#include <oran/io/fingerprint.hpp>
#include <oran/tool/registry.hpp>
#include <oran/tool/workspace.hpp>

#include "version_token.hpp"

namespace orangutan::tool {

namespace {

constexpr std::string_view kFileEditSchema =
    R"({"type":"object","properties":{"path":{"type":"string"},"old_string":{"type":"string"},)"
    R"("new_string":{"type":"string"},"replace_all":{"type":"boolean"},)"
    R"("max_bytes":{"type":"integer","minimum":1,"maximum":16777216},)"
    R"("expected_version":{"type":"string"}},)"
    R"("required":["path","old_string","new_string"],"additionalProperties":false})";

/// Hard ceiling for text mutation payloads. Mirrors the current
/// `io::ReadTextOptions::max_bytes` default so `file.edit` cannot write a file
/// larger than a follow-up `file.read` can ingest.
constexpr std::uintmax_t kMaxWriteBytes = 16U * 1024U * 1024U;

/// Indexes of every non-overlapping occurrence of `needle` in `haystack`, in
/// order. Non-overlapping is the natural fit for "replace": chained matches in
/// the input string get rewritten consistently with what a left-to-right scan
/// produces.
[[nodiscard]] std::vector<std::size_t> find_occurrences(std::string_view haystack, std::string_view needle) {
  std::vector<std::size_t> positions;
  for (std::size_t pos = haystack.find(needle); pos != std::string_view::npos;
       pos = haystack.find(needle, pos + needle.size())) {
    positions.push_back(pos);
  }
  return positions;
}

[[nodiscard]] core::Result<std::uintmax_t> parse_max_bytes(const nlohmann::json& parsed) {
  if (!parsed.contains("max_bytes")) {
    return kMaxWriteBytes;
  }

  const auto& raw = parsed["max_bytes"];
  if (!raw.is_number_integer() || raw.is_number_float()) {
    return std::unexpected(core::Error::invalid_argument("file.edit: `max_bytes` must be a positive integer"));
  }

  std::uint64_t value = 0U;
  if (raw.is_number_unsigned()) {
    value = raw.get<std::uint64_t>();
  } else {
    const auto signed_value = raw.get<std::int64_t>();
    if (signed_value <= 0) {
      return std::unexpected(core::Error::invalid_argument("file.edit: `max_bytes` must be between 1 and 16777216")
                                 .with("value", std::to_string(signed_value)));
    }
    value = static_cast<std::uint64_t>(signed_value);
  }

  if (value == 0U || value > kMaxWriteBytes) {
    return std::unexpected(core::Error::invalid_argument("file.edit: `max_bytes` must be between 1 and 16777216")
                               .with("value", std::to_string(value))
                               .with("max_bytes", std::to_string(kMaxWriteBytes)));
  }
  return static_cast<std::uintmax_t>(value);
}

[[nodiscard]] core::Result<std::size_t>
replacement_size(std::size_t source_size, std::size_t old_size, std::size_t new_size, std::size_t replacement_count) {
  if (new_size <= old_size) {
    return source_size - ((old_size - new_size) * replacement_count);
  }

  const auto delta = new_size - old_size;
  const auto available = std::numeric_limits<std::size_t>::max() - source_size;
  if (replacement_count > available / delta) {
    return std::unexpected(core::Error::invalid_argument("file.edit: replacement output is too large"));
  }
  return source_size + (delta * replacement_count);
}

/// Rebuilds the contents by stitching the unchanged slices around each match
/// with `new_string`. Returning a fresh `std::string` rather than mutating in
/// place keeps the substitution constant-time per match and avoids the
/// degenerate behaviour of repeated `string::replace` on overlapping
/// positions.
[[nodiscard]] std::string apply_replacements(std::string_view source,
                                             std::string_view old_string,
                                             std::string_view new_string,
                                             const std::vector<std::size_t>& positions) {
  std::string out;
  out.reserve(source.size() +
              (new_string.size() > old_string.size() ? (new_string.size() - old_string.size()) * positions.size() : 0));
  std::size_t cursor = 0;
  for (const auto pos : positions) {
    out.append(source.data() + cursor, pos - cursor);
    out.append(new_string);
    cursor = pos + old_string.size();
  }
  out.append(source.data() + cursor, source.size() - cursor);
  return out;
}

[[nodiscard]] async::Awaitable<core::Result<Output>> file_edit_handler(std::string_view input_json,
                                                                       DispatchContext& ctx) {
  nlohmann::json parsed;
  try {
    parsed = nlohmann::json::parse(input_json);
  } catch (const nlohmann::json::parse_error& e) {
    co_return std::unexpected(
        core::Error::invalid_argument("file.edit: input is not valid JSON").with("detail", e.what()));
  } catch (const std::exception& e) {
    co_return std::unexpected(
        core::Error::invalid_argument("file.edit: input is not valid JSON").with("detail", e.what()));
  }

  if (!parsed.is_object()) {
    co_return std::unexpected(core::Error::invalid_argument("file.edit: input must be a JSON object"));
  }
  if (!parsed.contains("path") || !parsed["path"].is_string()) {
    co_return std::unexpected(core::Error::invalid_argument("file.edit: input must include a string `path` field"));
  }
  if (!parsed.contains("old_string") || !parsed["old_string"].is_string()) {
    co_return std::unexpected(
        core::Error::invalid_argument("file.edit: input must include a string `old_string` field"));
  }
  if (!parsed.contains("new_string") || !parsed["new_string"].is_string()) {
    co_return std::unexpected(
        core::Error::invalid_argument("file.edit: input must include a string `new_string` field"));
  }

  bool replace_all = false;
  if (parsed.contains("replace_all")) {
    if (!parsed["replace_all"].is_boolean()) {
      co_return std::unexpected(core::Error::invalid_argument("file.edit: `replace_all` must be a boolean"));
    }
    replace_all = parsed["replace_all"].get<bool>();
  }

  std::optional<std::string> expected_version;
  if (parsed.contains("expected_version")) {
    if (!parsed["expected_version"].is_string()) {
      co_return std::unexpected(core::Error::invalid_argument("file.edit: `expected_version` must be a string"));
    }
    expected_version = parsed["expected_version"].get<std::string>();
  }

  auto max_bytes = parse_max_bytes(parsed);
  if (!max_bytes) {
    co_return std::unexpected(std::move(max_bytes).error());
  }

  auto path = ctx.resolved_path.has_value() ? ctx.resolved_path->absolute_path : parsed["path"].get<std::string>();
  auto old_string = parsed["old_string"].get<std::string>();
  auto new_string = parsed["new_string"].get<std::string>();

  if (old_string.empty()) {
    co_return std::unexpected(core::Error::invalid_argument("file.edit: `old_string` must be non-empty"));
  }
  if (old_string == new_string) {
    co_return std::unexpected(core::Error::invalid_argument("file.edit: `old_string` and `new_string` are identical"));
  }

  if (!ctx.resolved_path.has_value() && ctx.workspace != nullptr) {
    auto resolved = ctx.workspace->resolve_write(path, WriteIntent{.disposition = WriteDisposition::truncate});
    if (!resolved) {
      co_return std::unexpected(std::move(resolved).error());
    }
    path = std::move(resolved->absolute_path);
  }

  // Pre-edit fingerprint check: a stale `expected_version` aborts before
  // the read so the caller never observes a partial edit. The downstream
  // read still re-fingerprints internally for mid-read race detection;
  // this guard is the *intentional* freshness contract the agent asked for.
  if (expected_version) {
    auto pre = io::compute_file_fingerprint(path);
    if (!pre) {
      co_return std::unexpected(core::Error{core::ErrorKind::conflict, "file.edit: expected_version cannot be verified"}
                                    .with("path", path)
                                    .with("reason", "stale_fingerprint")
                                    .with("detail", std::string{pre.error().message()}));
    }
    const auto current_token = detail::version_token(path, *pre);
    if (*expected_version != current_token) {
      co_return std::unexpected(
          core::Error{core::ErrorKind::conflict, "file.edit: file has changed since the expected version"}
              .with("path", path)
              .with("reason", "stale_fingerprint")
              .with("expected", *expected_version)
              .with("fingerprint", current_token));
    }
  }

  auto contents = co_await io::read_text_file(ctx.executor, path, io::ReadTextOptions{.max_bytes = *max_bytes});
  if (!contents) {
    co_return std::unexpected(std::move(contents).error());
  }

  const auto positions = find_occurrences(*contents, old_string);
  if (positions.empty()) {
    co_return std::unexpected(
        core::Error::not_found("file.edit: `old_string` does not occur in the file").with("path", path));
  }
  if (positions.size() > 1 && !replace_all) {
    co_return std::unexpected(
        core::Error{core::ErrorKind::conflict,
                    "file.edit: `old_string` is not unique; pass `replace_all` to apply to every match"}
            .with("path", path)
            .with("match_count", std::to_string(positions.size())));
  }

  const std::size_t applied = replace_all ? positions.size() : 1U;
  std::vector<std::size_t> target_positions;
  if (replace_all) {
    target_positions = positions;
  } else {
    target_positions = {positions.front()};
  }

  auto output_size = replacement_size(contents->size(), old_string.size(), new_string.size(), target_positions.size());
  if (!output_size) {
    co_return std::unexpected(std::move(output_size).error().with("path", path));
  }
  if (static_cast<std::uintmax_t>(*output_size) > *max_bytes) {
    co_return std::unexpected(core::Error::invalid_argument("file.edit: output exceeds max_bytes")
                                  .with("path", path)
                                  .with("output_bytes", std::to_string(*output_size))
                                  .with("max_bytes", std::to_string(*max_bytes)));
  }

  auto replaced = apply_replacements(*contents, old_string, new_string, target_positions);
  const auto replaced_bytes = replaced.size();
  io::WriteTextOptions write_opts{.mode = io::WriteMode::truncate, .atomic = true};
  auto written = co_await io::write_text_file(ctx.executor, std::move(path), std::move(replaced), write_opts);
  if (!written) {
    co_return std::unexpected(std::move(written).error());
  }

  co_return Output{
      .text = std::format("edited {}: {} replacement{}",
                          parsed["path"].get<std::string>(),
                          applied,
                          applied == 1U ? "" : "s"),
      .usage =
          ToolUsage{
              .bytes_read = contents->size(),
              .bytes_written = replaced_bytes,
              .files_touched = 1,
              .match_count = applied,
          },
  };
}

}  // namespace

core::Result<void> register_file_edit(Registry& registry) {
  core::ToolDef def{
      .name = std::string{kFileEditName},
      .description = "Edit a UTF-8 text file by replacing `old_string` with `new_string`. Input: "
                     "{\"path\": <string>, \"old_string\": <string>, \"new_string\": <string>, "
                     "\"replace_all\"?: bool (default false), \"max_bytes\"?: positive integer "
                     "<= 16777216 (default 16777216), \"expected_version\"?: <version token "
                     "from a prior `file.read`>}. By default the call fails with `conflict` "
                     "if `old_string` is not unique; pass `replace_all=true` to rewrite every "
                     "occurrence. When `expected_version` is supplied the call fails with "
                     "`conflict` (reason=stale_fingerprint, current `fingerprint` in context) "
                     "if the file's current version differs. Returns a brief confirmation "
                     "listing the number of replacements applied and fills usage with "
                     "bytes_read, bytes_written, files_touched, and match_count.",
      .input_schema_json = std::string{kFileEditSchema},
      .required_capabilities = {core::Capability::edit_file},
      .deferred = false,
      .category = "file",
  };
  return registry.add(std::move(def), &file_edit_handler);
}

}  // namespace orangutan::tool
