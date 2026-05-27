// src/oran-tool/parse_input.cpp — shared JSON input parsing for built-ins.

#include "_impl/parse_input.hpp"

#include <exception>
#include <expected>
#include <format>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <oran/core/error.hpp>

namespace orangutan::tool::detail {

core::Result<nlohmann::json> parse_input_object(std::string_view input_json, std::string_view tool_name) {
  nlohmann::json parsed;
  try {
    parsed = nlohmann::json::parse(input_json);
  } catch (const nlohmann::json::parse_error& e) {
    return std::unexpected(
        core::Error::invalid_argument(std::format("{}: input is not valid JSON", tool_name)).with("detail", e.what()));
  } catch (const std::exception& e) {
    return std::unexpected(
        core::Error::invalid_argument(std::format("{}: input is not valid JSON", tool_name)).with("detail", e.what()));
  }

  if (!parsed.is_object()) {
    return std::unexpected(core::Error::invalid_argument(std::format("{}: input must be a JSON object", tool_name)));
  }
  return parsed;
}

core::Result<std::string>
require_string_field(const nlohmann::json& input, std::string_view tool_name, std::string_view field) {
  const auto it = input.find(field);
  if (it == input.end() || !it->is_string()) {
    return std::unexpected(
        core::Error::invalid_argument(std::format("{}: input must include a string `{}` field", tool_name, field)));
  }
  return it->get<std::string>();
}

}  // namespace orangutan::tool::detail
