// src/oran-tool/file_read.cpp — `file.read` built-in (spec 0011 v1).
//
// v2 input shape: `{"path": <string>, "start_line"?, "line_count"?,
// "offset_bytes"?, "length_bytes"?, "max_bytes"?, "if_version"?}`. The
// line/byte range pair is mutually exclusive (caught both at schema
// validation in `Registry::add` and at handler time by
// `io::FileRange` itself). The response wraps the returned text in a
// single header line carrying the version token, the covered span, the
// returned byte count, and the truncated flag. Since slice 62, the same
// facts also ride in `Output::data_json` so downstream adapters and UIs do
// not have to re-parse the header line.
// `if_version` short-circuits to `Error::not_modified` (carrying the
// current token in context) when the supplied token matches the current
// fingerprint, so a cached caller does not re-pay the body bytes.

#include <oran/tool/builtins.hpp>

#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/capability.hpp>
#include <oran/core/error.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/io/file.hpp>
#include <oran/io/fingerprint.hpp>
#include <oran/io/range.hpp>
#include <oran/tool/registry.hpp>
#include <oran/tool/workspace.hpp>

#include "_impl/parse_input.hpp"
#include "version_token.hpp"

namespace orangutan::tool {

namespace {

constexpr std::string_view kFileReadSchema =
    R"({"type":"object","properties":{"path":{"type":"string"},)"
    R"("start_line":{"type":"integer","minimum":1},"line_count":{"type":"integer","minimum":1},)"
    R"("offset_bytes":{"type":"integer","minimum":1},"length_bytes":{"type":"integer","minimum":1},)"
    R"("max_bytes":{"type":"integer","minimum":1,"maximum":16777216},)"
    R"("if_version":{"type":"string"}},)"
    R"("required":["path"],"additionalProperties":false})";

constexpr std::uintmax_t kMaxReadBytes = 16U * 1024U * 1024U;

[[nodiscard]] core::Result<std::uintmax_t> parse_positive_unsigned(const nlohmann::json& raw, std::string_view field) {
  if (!raw.is_number_integer() || raw.is_number_float()) {
    return std::unexpected(
        core::Error::invalid_argument(std::format("file.read: `{}` must be a positive integer", field)));
  }
  std::uint64_t value = 0U;
  if (raw.is_number_unsigned()) {
    value = raw.get<std::uint64_t>();
  } else {
    const auto signed_value = raw.get<std::int64_t>();
    if (signed_value <= 0) {
      return std::unexpected(core::Error::invalid_argument(std::format("file.read: `{}` must be positive", field))
                                 .with("value", std::to_string(signed_value)));
    }
    value = static_cast<std::uint64_t>(signed_value);
  }
  if (value == 0U) {
    return std::unexpected(core::Error::invalid_argument(std::format("file.read: `{}` must be positive", field)));
  }
  return static_cast<std::uintmax_t>(value);
}

[[nodiscard]] core::Result<io::ReadTextOptions> parse_options(const nlohmann::json& parsed) {
  io::ReadTextOptions options{};

  if (parsed.contains("max_bytes")) {
    auto mb = parse_positive_unsigned(parsed["max_bytes"], "max_bytes");
    if (!mb) {
      return std::unexpected(std::move(mb).error());
    }
    if (*mb > kMaxReadBytes) {
      return std::unexpected(core::Error::invalid_argument("file.read: `max_bytes` must be <= 16777216")
                                 .with("value", std::to_string(*mb))
                                 .with("max_bytes", std::to_string(kMaxReadBytes)));
    }
    options.max_bytes = *mb;
  }

  const bool has_line = parsed.contains("start_line") || parsed.contains("line_count");
  const bool has_byte = parsed.contains("offset_bytes") || parsed.contains("length_bytes");
  if (has_line && has_byte) {
    return std::unexpected(core::Error::invalid_argument("file.read: line range (start_line/line_count) and byte range "
                                                         "(offset_bytes/length_bytes) are mutually exclusive"));
  }

  if (has_line) {
    io::FileRange::LineSpan span{};
    if (parsed.contains("start_line")) {
      auto v = parse_positive_unsigned(parsed["start_line"], "start_line");
      if (!v) {
        return std::unexpected(std::move(v).error());
      }
      span.start_line = static_cast<std::uint64_t>(*v);
    } else {
      span.start_line = 1U;
    }
    if (parsed.contains("line_count")) {
      auto v = parse_positive_unsigned(parsed["line_count"], "line_count");
      if (!v) {
        return std::unexpected(std::move(v).error());
      }
      span.line_count = static_cast<std::uint64_t>(*v);
    } else {
      return std::unexpected(
          core::Error::invalid_argument("file.read: `line_count` is required when `start_line` is supplied"));
    }
    options.range = io::FileRange{.lines = span};
  } else if (has_byte) {
    io::FileRange::ByteSpan span{};
    if (parsed.contains("offset_bytes")) {
      auto v = parse_positive_unsigned(parsed["offset_bytes"], "offset_bytes");
      if (!v) {
        return std::unexpected(std::move(v).error());
      }
      span.offset_bytes = *v;
    } else {
      return std::unexpected(
          core::Error::invalid_argument("file.read: `offset_bytes` is required when `length_bytes` is supplied"));
    }
    if (parsed.contains("length_bytes")) {
      auto v = parse_positive_unsigned(parsed["length_bytes"], "length_bytes");
      if (!v) {
        return std::unexpected(std::move(v).error());
      }
      span.length_bytes = *v;
    } else {
      return std::unexpected(
          core::Error::invalid_argument("file.read: `length_bytes` is required when `offset_bytes` is supplied"));
    }
    options.range = io::FileRange{.bytes = span};
  }

  return options;
}

/// Header line embedded above the file body so the text-only `tool::Output`
/// can still surface the metadata callers need. Shape:
/// `<path>:<start_line>-<end_line> fingerprint=<token> bytes=<n>[ truncated]`.
[[nodiscard]] std::string
format_header(std::string_view path, const io::ReadTextResult& result, const std::string& token) {
  std::string header = std::format("{}:{}-{} fingerprint={} bytes={}",
                                   path,
                                   result.start_line,
                                   result.end_line,
                                   token,
                                   result.returned_bytes);
  if (result.truncated) {
    header.append(" truncated");
  }
  return header;
}

[[nodiscard]] std::string format_data_json(std::string_view path,
                                           std::string_view body,
                                           const io::ReadTextResult& result,
                                           const std::string& token) {
  return nlohmann::json{
      {"kind", "file_read"},
      {"path", std::string{path}},
      {"text", std::string{body}},
      {"fingerprint", token},
      {"start_line", result.start_line},
      {"end_line", result.end_line},
      {"returned_bytes", result.returned_bytes},
      {"truncated", result.truncated},
  }
      .dump();
}

[[nodiscard]] async::Awaitable<core::Result<Output>> file_read_handler(std::string_view input_json,
                                                                       DispatchContext& ctx) {
  auto parsed = detail::parse_input_object(input_json, kFileReadName);
  if (!parsed) {
    co_return std::unexpected(std::move(parsed).error());
  }

  auto path_field = detail::require_string_field(*parsed, kFileReadName, "path");
  if (!path_field) {
    co_return std::unexpected(std::move(path_field).error());
  }

  auto options = parse_options(*parsed);
  if (!options) {
    co_return std::unexpected(std::move(options).error());
  }

  std::optional<std::string> if_version;
  if (parsed->contains("if_version")) {
    if (!(*parsed)["if_version"].is_string()) {
      co_return std::unexpected(core::Error::invalid_argument("file.read: `if_version` must be a string"));
    }
    if_version = (*parsed)["if_version"].get<std::string>();
  }

  auto path = ctx.resolved_path.has_value() ? ctx.resolved_path->absolute_path : *std::move(path_field);
  if (!ctx.resolved_path.has_value() && ctx.workspace != nullptr) {
    auto resolved = ctx.workspace->resolve_read(path);
    if (!resolved) {
      co_return std::unexpected(std::move(resolved).error());
    }
    path = std::move(resolved->absolute_path);
  }

  // Short-circuit on `if_version` before the body read so cached callers
  // do not re-pay the IO. The pre-read fingerprint is the cheap stat-only
  // call; the full read still re-fingerprints internally for mid-read
  // race detection.
  if (if_version) {
    auto pre = io::compute_file_fingerprint(path);
    if (!pre) {
      co_return std::unexpected(std::move(pre).error());
    }
    const auto current_token = detail::version_token(path, *pre);
    if (*if_version == current_token) {
      co_return std::unexpected(core::Error::not_modified("file.read: file is unchanged since the supplied version")
                                    .with("path", path)
                                    .with("fingerprint", current_token));
    }
  }

  auto result = co_await io::read_text_file_ranged(ctx.executor, path, *options);
  if (!result) {
    co_return std::unexpected(std::move(result).error());
  }

  const auto token = detail::version_token(path, result->fingerprint);
  const auto header = format_header(path, *result, token);
  auto body = std::move(result->text);
  auto data_json = format_data_json(path, body, *result, token);
  std::string text = header;
  text.push_back('\n');
  text.append(body);
  co_return Output{
      .text = std::move(text),
      .data_json = std::move(data_json),
      .usage =
          ToolUsage{
              .bytes_read = result->returned_bytes,
              .files_touched = 1,
              .truncated = result->truncated,
          },
  };
}

}  // namespace

core::Result<void> register_file_read(Registry& registry) {
  core::ToolDef def{
      .name = std::string{kFileReadName},
      .description = "Read a UTF-8 text file from the host filesystem. Input: "
                     "{\"path\": <string>, \"start_line\"?: positive integer, \"line_count\"?: positive integer, "
                     "\"offset_bytes\"?: positive integer, \"length_bytes\"?: positive integer, "
                     "\"max_bytes\"?: positive integer <= 16777216 (default 16777216), "
                     "\"if_version\"?: <version token from a prior read>}. The line range "
                     "(start_line/line_count) and byte range (offset_bytes/length_bytes) "
                     "are mutually exclusive. When `if_version` matches the current file "
                     "fingerprint the call short-circuits with `not_modified`; otherwise "
                     "the output is a single header line "
                     "`<path>:<start_line>-<end_line> fingerprint=<token> bytes=<n>[ truncated]` "
                     "followed by the requested file slice on the next line; `data_json` carries "
                     "kind, path, text, fingerprint, start_line, end_line, returned_bytes, and truncated.",
      .input_schema_json = std::string{kFileReadSchema},
      .required_capabilities = {core::Capability::read_file},
      .deferred = false,
      .category = "file",
  };
  return registry.add(std::move(def), &file_read_handler);
}

}  // namespace orangutan::tool
