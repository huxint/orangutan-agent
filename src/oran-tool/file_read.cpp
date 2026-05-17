// src/oran-tool/file_read.cpp — `file.read` built-in.

#include <oran/tool/builtins.hpp>

#include <exception>
#include <expected>
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

namespace orangutan::tool {

namespace {

constexpr std::string_view kFileReadSchema =
    R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"],"additionalProperties":false})";

[[nodiscard]] async::Awaitable<core::Result<Output>> file_read_handler(std::string_view input_json,
                                                                       DispatchContext& ctx) {
  nlohmann::json parsed;
  try {
    parsed = nlohmann::json::parse(input_json);
  } catch (const nlohmann::json::parse_error& e) {
    co_return std::unexpected(
        core::Error::invalid_argument("file.read: input is not valid JSON").with("detail", e.what()));
  } catch (const std::exception& e) {
    co_return std::unexpected(
        core::Error::invalid_argument("file.read: input is not valid JSON").with("detail", e.what()));
  }

  if (!parsed.is_object() || !parsed.contains("path") || !parsed["path"].is_string()) {
    co_return std::unexpected(
        core::Error::invalid_argument("file.read: input must be an object with a string `path` field"));
  }

  auto path = parsed["path"].get<std::string>();
  auto contents = co_await io::read_text_file(ctx.executor, std::move(path));
  if (!contents) {
    co_return std::unexpected(std::move(contents).error());
  }
  co_return Output{.text = std::move(*contents)};
}

}  // namespace

core::Result<void> register_file_read(Registry& registry) {
  core::ToolDef def{
      .name = std::string{kFileReadName},
      .description = "Read a UTF-8 text file from the host filesystem. Input: {\"path\": <string>}. "
                     "Returns the file contents verbatim.",
      .input_schema_json = std::string{kFileReadSchema},
      .required_capabilities = {core::Capability::read_file},
  };
  return registry.add(std::move(def), &file_read_handler);
}

}  // namespace orangutan::tool
