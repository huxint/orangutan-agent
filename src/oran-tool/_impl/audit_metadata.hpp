// Internal audit metadata helpers for tool dispatch.

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <oran/hook/decision.hpp>
#include <oran/tool/output.hpp>

namespace orangutan::tool::detail {

[[nodiscard]] std::optional<std::string> with_usage_metadata(std::string_view metadata_json, const ToolUsage& usage);

[[nodiscard]] std::string with_hook_decision_metadata(std::string_view metadata_json,
                                                      std::span<const hook::HookDecisionTrace> trace,
                                                      std::optional<std::string> original_input_hash = std::nullopt,
                                                      std::optional<std::string> rewritten_input_hash = std::nullopt);

}  // namespace orangutan::tool::detail
