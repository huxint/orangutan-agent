// Internal JSON input parsing helpers for `oran-tool` built-ins.
//
// Built-in tools share the same two preludes: parse the input bytes as
// JSON and require a top-level object, then pull required string fields
// out of it. Centralising both lets each handler skip ~10 lines of
// boilerplate try/catch + is_object/is_string checks and keeps the
// error-message vocabulary uniform across the catalog.

#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <oran/core/result.hpp>

namespace orangutan::tool::detail {

/// Parse `input_json` and require the value to be a JSON object.
///
/// Error shapes:
///   - `invalid_argument` with message `"<tool_name>: input is not valid JSON"`
///     and `detail=<parser message>` when the bytes do not parse as JSON
///     (catches `nlohmann::json::parse_error` and any other `std::exception`).
///   - `invalid_argument` with message `"<tool_name>: input must be a JSON object"`
///     when the bytes parse but the top-level value is not an object.
[[nodiscard]] core::Result<nlohmann::json> parse_input_object(std::string_view input_json, std::string_view tool_name);

/// Read a required string field from a parsed input object.
///
/// Error: `invalid_argument` with message
/// `"<tool_name>: input must include a string `<field>` field"`
/// when the field is missing or not a JSON string.
[[nodiscard]] core::Result<std::string>
require_string_field(const nlohmann::json& input, std::string_view tool_name, std::string_view field);

}  // namespace orangutan::tool::detail
