// src/oran-tool/audit_metadata.cpp — structured audit metadata helpers.

#include "_impl/audit_metadata.hpp"

#include <chrono>
#include <exception>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <oran/core/enum_names.hpp>
#include <oran/hook/decision.hpp>
#include <oran/hook/event.hpp>
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

[[nodiscard]] nlohmann::json hook_decision_trace_to_json(const hook::HookDecisionTrace& decision) {
  auto row = nlohmann::json::object();
  row["sink_id"] = decision.sink_id;
  row["kind"] = std::string{core::enum_name(decision.kind)};
  row["reason"] = decision.reason;
  if (decision.elapsed.has_value()) {
    row["elapsed_ms"] = decision.elapsed->count();
  }
  return row;
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

std::string with_hook_decision_metadata(std::string_view metadata_json,
                                        std::span<const hook::HookDecisionTrace> trace,
                                        std::optional<std::string> original_input_hash,
                                        std::optional<std::string> rewritten_input_hash) {
  auto metadata = parse_metadata_object(metadata_json);
  auto rows = nlohmann::json::array();
  for (const auto& decision : trace) {
    rows.push_back(hook_decision_trace_to_json(decision));
  }
  metadata["hook_decisions"] = std::move(rows);
  if (original_input_hash.has_value()) {
    metadata["original_input_hash"] = std::move(*original_input_hash);
  }
  if (rewritten_input_hash.has_value()) {
    metadata["rewritten_input_hash"] = std::move(*rewritten_input_hash);
  }
  return metadata.dump();
}

std::string with_permission_ask_metadata(std::string_view metadata_json,
                                         std::span<const hook::HookDecisionTrace> trace) {
  auto metadata = parse_metadata_object(metadata_json);
  auto rows = nlohmann::json::array();
  for (const auto& decision : trace) {
    rows.push_back(hook_decision_trace_to_json(decision));
  }
  metadata["permission_ask_decisions"] = std::move(rows);
  return metadata.dump();
}

std::string hook_publish_metadata_json(hook::Event event,
                                       const hook::HookDecisionTrace& winning_trace,
                                       std::span<const hook::HookDecisionTrace> trace) {
  auto metadata = nlohmann::json::object();
  metadata["event"] = std::string{core::enum_name(event)};
  metadata["sink_id"] = winning_trace.sink_id;
  metadata["decision_kind"] = std::string{core::enum_name(winning_trace.kind)};
  metadata["reason"] = winning_trace.reason;
  if (winning_trace.reason.starts_with("hook_error")) {
    metadata["error"] = winning_trace.reason;
  }
  if (winning_trace.elapsed.has_value()) {
    metadata["elapsed_ms"] = winning_trace.elapsed->count();
  }
  auto rows = nlohmann::json::array();
  for (const auto& decision : trace) {
    rows.push_back(hook_decision_trace_to_json(decision));
  }
  metadata["hook_decisions"] = std::move(rows);
  return metadata.dump();
}

}  // namespace orangutan::tool::detail
