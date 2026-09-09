#include <oran/tool/builtins.hpp>

#include <array>
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
    R"("if_version":{"type":"string"},"allow_outside_workspace":{"type":"boolean"}},)"
    R"("required":["path"],"additionalProperties":false})";

struct FileReadRequest {
  std::string path;
  io::ReadTextOptions options;
  std::optional<std::string> if_version;
};

[[nodiscard]] core::Result<io::ReadTextOptions> parse_options(const nlohmann::json& parsed) {
  io::ReadTextOptions options{};

  auto max_bytes = detail::parse_file_max_bytes(parsed, kFileReadName);
  if (!max_bytes) {
    return std::unexpected(std::move(max_bytes).error());
  }
  options.max_bytes = *max_bytes;

  const bool has_line = parsed.contains("start_line") || parsed.contains("line_count");
  const bool has_byte = parsed.contains("offset_bytes") || parsed.contains("length_bytes");
  if (has_line && has_byte) {
    return std::unexpected(core::Error::invalid_argument("FileRead: line range (start_line/line_count) and byte range "
                                                         "(offset_bytes/length_bytes) are mutually exclusive"));
  }

  if (has_line) {
    io::FileRange::LineSpan span{};
    if (parsed.contains("start_line")) {
      auto v = detail::parse_positive_unsigned(parsed["start_line"], kFileReadName, "start_line");
      if (!v) {
        return std::unexpected(std::move(v).error());
      }
      span.start_line = static_cast<std::uint64_t>(*v);
    } else {
      span.start_line = 1U;
    }
    if (parsed.contains("line_count")) {
      auto v = detail::parse_positive_unsigned(parsed["line_count"], kFileReadName, "line_count");
      if (!v) {
        return std::unexpected(std::move(v).error());
      }
      span.line_count = static_cast<std::uint64_t>(*v);
    } else {
      return std::unexpected(
          core::Error::invalid_argument("FileRead: `line_count` is required when `start_line` is supplied"));
    }
    options.range = io::FileRange{.lines = span};
  } else if (has_byte) {
    io::FileRange::ByteSpan span{};
    if (parsed.contains("offset_bytes")) {
      auto v = detail::parse_positive_unsigned(parsed["offset_bytes"], kFileReadName, "offset_bytes");
      if (!v) {
        return std::unexpected(std::move(v).error());
      }
      span.offset_bytes = *v;
    } else {
      return std::unexpected(
          core::Error::invalid_argument("FileRead: `offset_bytes` is required when `length_bytes` is supplied"));
    }
    if (parsed.contains("length_bytes")) {
      auto v = detail::parse_positive_unsigned(parsed["length_bytes"], kFileReadName, "length_bytes");
      if (!v) {
        return std::unexpected(std::move(v).error());
      }
      span.length_bytes = *v;
    } else {
      return std::unexpected(
          core::Error::invalid_argument("FileRead: `length_bytes` is required when `offset_bytes` is supplied"));
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

[[nodiscard]] async::Awaitable<core::Result<Output>> file_read_handler(FileReadRequest request,
                                                                      DispatchContext& ctx) {
  auto& [path, options, if_version] = request;
  std::optional<io::ReadOnlyFile> authorized_file;
  if (ctx.resolved_path.has_value()) {
    if (!ctx.resolved_path->authority.has_value()) {
      co_return std::unexpected(core::Error::internal("FileRead: resolved workspace path is missing authority"));
    }
    path = ctx.resolved_path->absolute_path;
    auto opened = ctx.resolved_path->authority->open_file(io::AnchoredPath{
        .relative_path = ctx.resolved_path->authority_relative_path,
        .symlink_policy = io::AnchoredSymlinkPolicy::allow_beneath,
    });
    if (!opened) {
      co_return std::unexpected(std::move(opened).error());
    }
    authorized_file.emplace(std::move(*opened));
  } else if (ctx.workspace != nullptr) {
    co_return std::unexpected(
        core::Error::internal("FileRead: workspace dispatch did not provide a resolved authority"));
  }

  // Short-circuit on `if_version` before the body read so cached callers
  // do not re-pay the IO. The pre-read fingerprint is the cheap stat-only
  // call; the full read still re-fingerprints internally for mid-read
  // race detection.
  if (if_version) {
    auto pre = authorized_file.has_value() ? io::compute_file_fingerprint(*authorized_file)
                                           : io::compute_file_fingerprint(path);
    if (!pre) {
      co_return std::unexpected(std::move(pre).error());
    }
    const auto current_token = detail::version_token(path, *pre);
    if (*if_version == current_token) {
      co_return std::unexpected(core::Error::not_modified("FileRead: file is unchanged since the supplied version")
                                    .with("path", path)
                                    .with("fingerprint", current_token));
    }
  }

  auto result = authorized_file.has_value()
                    ? co_await io::read_text_file_ranged(ctx.executor, std::move(*authorized_file), options)
                    : co_await io::read_text_file_ranged(ctx.executor, path, options);
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

[[nodiscard]] core::Result<PreparedCall> prepare_file_read(std::string_view input_json) {
  constexpr auto fields = std::to_array<std::string_view>({
      "path", "start_line", "line_count", "offset_bytes", "length_bytes", "max_bytes", "if_version",
      "allow_outside_workspace"});
  auto parsed = detail::parse_input_object(input_json, kFileReadName, fields);
  if (!parsed) {
    return std::unexpected(std::move(parsed).error());
  }

  auto path_field = detail::require_path_field(*parsed, kFileReadName);
  if (!path_field) {
    return std::unexpected(std::move(path_field).error());
  }

  auto options = parse_options(*parsed);
  if (!options) {
    return std::unexpected(std::move(options).error());
  }

  std::optional<std::string> if_version;
  if (parsed->contains("if_version")) {
    if (!(*parsed)["if_version"].is_string()) {
      return std::unexpected(core::Error::invalid_argument("FileRead: `if_version` must be a string"));
    }
    if_version = (*parsed)["if_version"].get<std::string>();
  }
  if (parsed->contains("allow_outside_workspace") && !(*parsed)["allow_outside_workspace"].is_boolean()) {
    return std::unexpected(core::Error::invalid_argument("FileRead: `allow_outside_workspace` must be a boolean"));
  }

  return PreparedCall{
      .path = PathRequest{.path = *path_field,
                          .intent = PathIntent::read,
                          .allow_outside_workspace = parsed->value("allow_outside_workspace", false)},
      .execute = [request = FileReadRequest{std::move(*path_field), *options, std::move(if_version)}](
                     DispatchContext& ctx) mutable { return file_read_handler(std::move(request), ctx); },
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
                     "\"if_version\"?: <version token from a prior read>, "
                     "\"allow_outside_workspace\"?: bool (default false; requires approval)}. The line range "
                     "(start_line/line_count) and byte range (offset_bytes/length_bytes) "
                     "are mutually exclusive. When `if_version` matches the current file "
                     "fingerprint the call short-circuits with `not_modified`; otherwise "
                     "the output is a single header line "
                     "`<path>:<start_line>-<end_line> fingerprint=<token> bytes=<n>[ truncated]` "
                     "followed by the requested file slice on the next line; `data_json` carries "
                     "kind, path, text, fingerprint, start_line, end_line, returned_bytes, and truncated.",
      .input_schema_json = std::string{kFileReadSchema},
      .required_capabilities = {core::Capability::read_file},
  };
  return registry.add_prepared(std::move(def), &prepare_file_read);
}

}  // namespace orangutan::tool
