// include/oran/core/role.hpp — message author identity.
//
// `Role` is the author of a `core::Message`. Stable string mapping so wire
// formats and logs agree; `std::formatter` so callers can `std::print` the
// value directly.

#pragma once

#include <cstdint>
#include <format>
#include <optional>
#include <string_view>

namespace orangutan::core {

enum class Role : std::uint8_t {
  user,
  assistant,
  system,
  tool,
};

[[nodiscard]] std::string_view to_string_view(Role) noexcept;

/// Inverse of `to_string_view`. Returns `std::nullopt` when `text` does not
/// name a known role. `noexcept` so it can be called from any context.
[[nodiscard]] std::optional<Role> parse_role(std::string_view text) noexcept;

}  // namespace orangutan::core

template <>
struct std::formatter<orangutan::core::Role> : std::formatter<std::string_view> {
  template <class FormatContext>
  auto format(orangutan::core::Role r, FormatContext& ctx) const {
    return std::formatter<std::string_view>::format(orangutan::core::to_string_view(r), ctx);
  }
};
