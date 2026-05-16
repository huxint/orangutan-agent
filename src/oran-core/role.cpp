// src/oran-core/role.cpp — stable string mapping for `core::Role`.

#include <oran/core/role.hpp>

#include <array>
#include <cstddef>
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

}  // namespace orangutan::core
