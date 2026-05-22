// src/oran-tool/file_write.cpp — `file.write` built-in.

#include <oran/tool/builtins.hpp>

#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/capability.hpp>
#include <oran/core/error.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/io/file.hpp>
#include <oran/tool/registry.hpp>
#include <oran/tool/workspace.hpp>

namespace orangutan::tool {

namespace {

constexpr std::string_view kFileWriteSchema =
    R"({"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"},)"
    R"("mode":{"type":"string","enum":["truncate","append","fail_if_exists"]},)"
    R"("create_parents":{"type":"boolean"},"max_bytes":{"type":"integer","minimum":1,"maximum":16777216}},)"
    R"("required":["path","content"],"additionalProperties":false})";

/// Hard ceiling for text mutation payloads. Mirrors the current
/// `io::ReadTextOptions::max_bytes` default so write/edit tools cannot create
/// files larger than the read side is willing to ingest in a later turn.
constexpr std::uintmax_t kMaxWriteBytes = 16U * 1024U * 1024U;

[[nodiscard]] core::Result<io::WriteMode> parse_write_mode(std::string_view text) {
  if (text == "truncate") {
    return io::WriteMode::truncate;
  }
  if (text == "append") {
    return io::WriteMode::append;
  }
  if (text == "fail_if_exists") {
    return io::WriteMode::fail_if_exists;
  }
  return std::unexpected(
      core::Error::invalid_argument("file.write: `mode` must be one of truncate|append|fail_if_exists")
          .with("value", std::string{text}));
}

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
  nlohmann::json parsed;
  try {
    parsed = nlohmann::json::parse(input_json);
  } catch (const nlohmann::json::parse_error& e) {
    co_return std::unexpected(
        core::Error::invalid_argument("file.write: input is not valid JSON").with("detail", e.what()));
  } catch (const std::exception& e) {
    co_return std::unexpected(
        core::Error::invalid_argument("file.write: input is not valid JSON").with("detail", e.what()));
  }

  if (!parsed.is_object()) {
    co_return std::unexpected(core::Error::invalid_argument("file.write: input must be a JSON object"));
  }
  if (!parsed.contains("path") || !parsed["path"].is_string()) {
    co_return std::unexpected(core::Error::invalid_argument("file.write: input must include a string `path` field"));
  }
  if (!parsed.contains("content") || !parsed["content"].is_string()) {
    co_return std::unexpected(core::Error::invalid_argument("file.write: input must include a string `content` field"));
  }

  auto max_bytes = parse_max_bytes(parsed);
  if (!max_bytes) {
    co_return std::unexpected(std::move(max_bytes).error());
  }

  io::WriteTextOptions options{};
  if (parsed.contains("mode")) {
    if (!parsed["mode"].is_string()) {
      co_return std::unexpected(core::Error::invalid_argument("file.write: `mode` must be a string"));
    }
    auto mode = parse_write_mode(parsed["mode"].get<std::string>());
    if (!mode) {
      co_return std::unexpected(std::move(mode).error());
    }
    options.mode = *mode;
  }
  if (parsed.contains("create_parents")) {
    if (!parsed["create_parents"].is_boolean()) {
      co_return std::unexpected(core::Error::invalid_argument("file.write: `create_parents` must be a boolean"));
    }
    options.create_parent_directories = parsed["create_parents"].get<bool>();
  }

  auto path = parsed["path"].get<std::string>();
  auto content = parsed["content"].get<std::string>();
  const auto byte_count = content.size();
  if (static_cast<std::uintmax_t>(byte_count) > *max_bytes) {
    co_return std::unexpected(core::Error::invalid_argument("file.write: `content` exceeds max_bytes")
                                  .with("path", path)
                                  .with("content_bytes", std::to_string(byte_count))
                                  .with("max_bytes", std::to_string(*max_bytes)));
  }
  if (ctx.workspace != nullptr) {
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
  // Truncate mode is the dominant "rewrite this file" call shape and the one
  // an LLM expects to be safe under partial-write failures; route it through
  // the temp-then-rename atomic path. Append and fail_if_exists keep their
  // existing semantics because the atomic path is incompatible with both.
  options.atomic = options.mode == io::WriteMode::truncate;
  auto written = co_await io::write_text_file(ctx.executor, path, std::move(content), options);
  if (!written) {
    co_return std::unexpected(std::move(written).error());
  }
  co_return Output{.text = std::format("wrote {} bytes to {}", byte_count, path)};
}

}  // namespace

core::Result<void> register_file_write(Registry& registry) {
  core::ToolDef def{
      .name = std::string{kFileWriteName},
      .description = "Write UTF-8 text content to a host filesystem path. Input: "
                     "{\"path\": <string>, \"content\": <string>, \"mode\"?: "
                     "\"truncate\"|\"append\"|\"fail_if_exists\" (default truncate), "
                     "\"create_parents\"?: bool (default false), \"max_bytes\"?: "
                     "positive integer <= 16777216 (default 16777216)}. Returns a brief "
                     "confirmation listing the number of bytes written.",
      .input_schema_json = std::string{kFileWriteSchema},
      .required_capabilities = {core::Capability::write_file},
  };
  return registry.add(std::move(def), &file_write_handler);
}

}  // namespace orangutan::tool
