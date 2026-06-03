// src/oran-tool/file_write.cpp — `file.write` built-in.

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
#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/io/file.hpp>
#include <oran/io/fingerprint.hpp>
#include <oran/tool/registry.hpp>
#include <oran/tool/workspace.hpp>

#include "_impl/parse_input.hpp"
#include "version_token.hpp"

namespace orangutan::tool {

namespace {

constexpr std::string_view kFileWriteSchema =
    R"({"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"},)"
    R"("mode":{"type":"string","enum":["truncate","append","fail_if_exists"]},)"
    R"("create_parents":{"type":"boolean"},"max_bytes":{"type":"integer","minimum":1,"maximum":16777216},)"
    R"("expected_version":{"type":"string"}},)"
    R"("required":["path","content"],"additionalProperties":false})";

/// Hard ceiling for text mutation payloads. Mirrors the current
/// `io::ReadTextOptions::max_bytes` default so write/edit tools cannot create
/// files larger than the read side is willing to ingest in a later turn.
constexpr std::uintmax_t kMaxWriteBytes = 16U * 1024U * 1024U;

[[nodiscard]] WriteDisposition to_write_disposition(io::WriteMode mode) noexcept {
  switch (mode) {
    case io::WriteMode::truncate:
      return WriteDisposition::truncate;
    case io::WriteMode::append:
      return WriteDisposition::append;
    case io::WriteMode::fail_if_exists:
      return WriteDisposition::fail_if_exists;
  }
  return WriteDisposition::truncate;
}

[[nodiscard]] core::Result<std::uintmax_t> parse_max_bytes(const nlohmann::json& parsed) {
  if (!parsed.contains("max_bytes")) {
    return kMaxWriteBytes;
  }

  const auto& raw = parsed["max_bytes"];
  if (!raw.is_number_integer() || raw.is_number_float()) {
    return std::unexpected(core::Error::invalid_argument("file.write: `max_bytes` must be a positive integer"));
  }

  std::uint64_t value = 0U;
  if (raw.is_number_unsigned()) {
    value = raw.get<std::uint64_t>();
  } else {
    const auto signed_value = raw.get<std::int64_t>();
    if (signed_value <= 0) {
      return std::unexpected(core::Error::invalid_argument("file.write: `max_bytes` must be between 1 and 16777216")
                                 .with("value", std::to_string(signed_value)));
    }
    value = static_cast<std::uint64_t>(signed_value);
  }

  if (value == 0U || value > kMaxWriteBytes) {
    return std::unexpected(core::Error::invalid_argument("file.write: `max_bytes` must be between 1 and 16777216")
                               .with("value", std::to_string(value))
                               .with("max_bytes", std::to_string(kMaxWriteBytes)));
  }
  return static_cast<std::uintmax_t>(value);
}

[[nodiscard]] async::Awaitable<core::Result<Output>> file_write_handler(std::string_view input_json,
                                                                        DispatchContext& ctx) {
  auto parsed = detail::parse_input_object(input_json, kFileWriteName);
  if (!parsed) {
    co_return std::unexpected(std::move(parsed).error());
  }

  auto path_field = detail::require_string_field(*parsed, kFileWriteName, "path");
  if (!path_field) {
    co_return std::unexpected(std::move(path_field).error());
  }
  auto content_field = detail::require_string_field(*parsed, kFileWriteName, "content");
  if (!content_field) {
    co_return std::unexpected(std::move(content_field).error());
  }

  auto max_bytes = parse_max_bytes(*parsed);
  if (!max_bytes) {
    co_return std::unexpected(std::move(max_bytes).error());
  }

  io::WriteTextOptions options{};
  if (parsed->contains("mode")) {
    if (!(*parsed)["mode"].is_string()) {
      co_return std::unexpected(core::Error::invalid_argument("file.write: `mode` must be a string"));
    }
    const auto mode_text = (*parsed)["mode"].get<std::string>();
    auto mode = core::parse_enum<io::WriteMode>(mode_text);
    if (!mode.has_value()) {
      co_return std::unexpected(
          core::Error::invalid_argument("file.write: `mode` must be one of truncate|append|fail_if_exists")
              .with("value", mode_text));
    }
    options.mode = *mode;
  }
  if (parsed->contains("create_parents")) {
    if (!(*parsed)["create_parents"].is_boolean()) {
      co_return std::unexpected(core::Error::invalid_argument("file.write: `create_parents` must be a boolean"));
    }
    options.create_parent_directories = (*parsed)["create_parents"].get<bool>();
  }

  std::optional<std::string> expected_version;
  if (parsed->contains("expected_version")) {
    if (!(*parsed)["expected_version"].is_string()) {
      co_return std::unexpected(core::Error::invalid_argument("file.write: `expected_version` must be a string"));
    }
    expected_version = (*parsed)["expected_version"].get<std::string>();
  }

  auto path = ctx.resolved_path.has_value() ? ctx.resolved_path->absolute_path : *std::move(path_field);
  auto content = *std::move(content_field);
  const auto byte_count = content.size();
  if (static_cast<std::uintmax_t>(byte_count) > *max_bytes) {
    co_return std::unexpected(core::Error::invalid_argument("file.write: `content` exceeds max_bytes")
                                  .with("path", path)
                                  .with("content_bytes", std::to_string(byte_count))
                                  .with("max_bytes", std::to_string(*max_bytes)));
  }
  if (!ctx.resolved_path.has_value() && ctx.workspace != nullptr) {
    auto resolved = ctx.workspace->resolve_write(path,
                                                 WriteIntent{
                                                     .disposition = to_write_disposition(options.mode),
                                                     .create_parent_directories = options.create_parent_directories,
                                                 });
    if (!resolved) {
      co_return std::unexpected(std::move(resolved).error());
    }
    path = std::move(resolved->absolute_path);
  }

  // Pre-write fingerprint check: a stale `expected_version` aborts the
  // mutation before the temp-then-rename so the caller observes a
  // consistent `conflict / stale_fingerprint` response instead of a
  // half-written file. A missing target is itself a mismatch — `file.write`
  // is meant to be paired with a recent `file.read`, so vanishing-from-disk
  // is the same "your token is stale, re-read" outcome.
  if (expected_version) {
    auto pre = io::compute_file_fingerprint(path);
    if (!pre) {
      co_return std::unexpected(
          core::Error{core::ErrorKind::conflict, "file.write: expected_version cannot be verified"}
              .with("path", path)
              .with("reason", "stale_fingerprint")
              .with("detail", std::string{pre.error().message()}));
    }
    const auto current_token = detail::version_token(path, *pre);
    if (*expected_version != current_token) {
      co_return std::unexpected(
          core::Error{core::ErrorKind::conflict, "file.write: file has changed since the expected version"}
              .with("path", path)
              .with("reason", "stale_fingerprint")
              .with("expected", *expected_version)
              .with("fingerprint", current_token));
    }
  }

  // Truncate mode is the dominant "rewrite this file" call shape and the one
  // an LLM expects to be safe under partial-write failures; route it through
  // the temp-then-rename atomic path. Append and fail_if_exists keep their
  // existing semantics because the atomic path is incompatible with both.
  options.atomic = options.mode == io::WriteMode::truncate;
  auto written = co_await io::write_text_file(ctx.executor, path, std::move(content), options);
  if (!written) {
    co_return std::unexpected(std::move(written).error());
  }
  co_return Output{
      .text = std::format("wrote {} bytes to {}", byte_count, path),
      .usage =
          ToolUsage{
              .bytes_written = byte_count,
              .files_touched = 1,
          },
  };
}

}  // namespace

core::Result<void> register_file_write(Registry& registry) {
  core::ToolDef def{
      .name = std::string{kFileWriteName},
      .description = "Write UTF-8 text content to a host filesystem path. Input: "
                     "{\"path\": <string>, \"content\": <string>, \"mode\"?: "
                     "\"truncate\"|\"append\"|\"fail_if_exists\" (default truncate), "
                     "\"create_parents\"?: bool (default false), \"max_bytes\"?: "
                     "positive integer <= 16777216 (default 16777216), "
                     "\"expected_version\"?: <version token from a prior `file.read`>}. "
                     "When `expected_version` is supplied the call fails with "
                     "`conflict` (reason=stale_fingerprint, current `fingerprint` in "
                     "context) if the file's current version differs. Returns a brief "
                     "confirmation listing the number of bytes written and fills usage "
                     "with bytes_written plus files_touched.",
      .input_schema_json = std::string{kFileWriteSchema},
      .required_capabilities = {core::Capability::write_file},
      .deferred = false,
      .category = "file",
  };
  return registry.add(std::move(def), &file_write_handler);
}

}  // namespace orangutan::tool
