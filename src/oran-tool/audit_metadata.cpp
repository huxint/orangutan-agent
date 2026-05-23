// src/oran-tool/audit_metadata.cpp — structured audit metadata helpers.

#include "_impl/audit_metadata.hpp"

#include <chrono>
#include <exception>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <oran/tool/output.hpp>

namespace orangutan::tool::detail {

namespace {

[[nodiscard]] nlohmann::json parse_metadata_object(std::string_view metadata_json) {
  try {
    auto parsed = nlohmann::json::parse(metadata_json);
    if (parsed.is_object()) {
      return parsed;
    }
  } catch (const nlohmann::json::parse_error&) {
  } catch (const std::exception&) {}
  return nlohmann::json::object();
}

[[nodiscard]] nlohmann::json usage_to_json(const ToolUsage& usage) {
  auto out = nlohmann::json::object();
  if (usage.bytes_read.has_value()) {
    out["bytes_read"] = *usage.bytes_read;
  }
  if (usage.bytes_written.has_value()) {
    out["bytes_written"] = *usage.bytes_written;
  }
  if (usage.files_touched.has_value()) {
    out["files_touched"] = *usage.files_touched;
  }
  if (usage.match_count.has_value()) {
    out["match_count"] = *usage.match_count;
  }
  if (usage.cost_estimate.has_value()) {
    out["cost_estimate"] = *usage.cost_estimate;
  }
  if (usage.wall_time.has_value()) {
    out["wall_time_ms"] = std::chrono::duration<double, std::milli>{*usage.wall_time}.count();
  }
  if (usage.truncated) {
    out["truncated"] = true;
  }
  if (usage.data_dropped) {
    out["data_dropped"] = true;
  }
  return out;
}

}  // namespace

std::optional<std::string> with_usage_metadata(std::string_view metadata_json, const ToolUsage& usage) {
  if (usage.empty()) {
    return std::nullopt;
  }
  auto metadata = parse_metadata_object(metadata_json);
  metadata["usage"] = usage_to_json(usage);
  return metadata.dump();
}

}  // namespace orangutan::tool::detail
