// include/oran/core/turn_id.hpp — turn correlation id.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

namespace orangutan::core {

/// Trace/audit correlation id for one agent turn.
///
/// The storage boundary persists this as a 16-byte BLOB. Generation is owned by
/// the future trace writer; core only owns the shared value shape so agent,
/// tool, permission, hook, and storage code do not invent incompatible ids.
using TurnId = std::array<std::byte, 16>;

[[nodiscard]] inline bool is_zero_turn_id(const TurnId& id) noexcept {
  return std::ranges::all_of(id, [](std::byte byte) { return byte == std::byte{}; });
}

}  // namespace orangutan::core
