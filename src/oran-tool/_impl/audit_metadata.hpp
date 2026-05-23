// Internal audit metadata helpers for tool dispatch.

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <oran/tool/output.hpp>

namespace orangutan::tool::detail {

[[nodiscard]] std::optional<std::string> with_usage_metadata(std::string_view metadata_json, const ToolUsage& usage);

}  // namespace orangutan::tool::detail
