// src/oran-tool/audit_metadata.cpp — structured audit metadata helpers.

#include "_impl/audit_metadata.hpp"

#include <exception>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <oran/core/enum_names.hpp>
#include <oran/hook/decision.hpp>

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

}  // namespace orangutan::tool::detail
