// src/oran-core/role.cpp — stable string mapping for `core::Role`.

#include <oran/core/role.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace orangutan::core {

namespace {

constexpr std::array<std::string_view, 4> kRoleNames{
    "user",
    "assistant",
    "system",
    "tool",
};

}  // namespace

std::string_view to_string_view(Role r) noexcept {
  const auto idx = static_cast<std::size_t>(r);
  if (idx < kRoleNames.size()) {
    return kRoleNames[idx];
  }
  return "unknown";
}

std::optional<Role> parse_role(std::string_view text) noexcept {
  for (std::size_t i = 0; i < kRoleNames.size(); ++i) {
    if (text == kRoleNames[i]) {
      return static_cast<Role>(i);
    }
  }
  return std::nullopt;
}

}  // namespace orangutan::core
