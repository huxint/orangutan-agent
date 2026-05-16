// include/oran/core/capability.hpp — runtime capability vocabulary.
//
// `Capability` is the v2 mechanism that ties tools (`oran-tool`) to
// permission rules (`oran-permission`). Tools declare the capabilities they
// need via `ToolDef::requires` (future slice); permission rules may scope
// to a capability via `Rule::capability` (next slice). The set
// representation (bitset / sorted span / flat-set) is intentionally left to
// the consumer — the universe is 18 entries today, small enough that a
// `std::uint32_t` bitset suffices when callers want one.
//
// The enum text matches the design doc verbatim
// (`docs/design-docs/tool-runtime.md`); breaking that mapping in a future
// change is a rule violation under `docs/rules/docs-in-sync.md`.

#pragma once

#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string_view>

namespace orangutan::core {

enum class Capability : std::uint8_t {
  // file system
  read_file,
  write_file,
  edit_file,
  delete_path,
  // network
  egress_http,
  egress_websocket,
  // process
  spawn_subprocess,
  signal_subprocess,
  // memory
  read_memory,
  write_memory,
  // orchestration
  spawn_agent,
  send_message_intra_team,
  send_message_inter_team,
  // automation
  schedule_job,
  modify_job,
  run_job_now,
  // skills
  invoke_skill,
  // misc
  external_mcp,
  runtime_loader,
};

/// Stable, identifier-style spelling for a `Capability`. Wire formats, audit
/// logs, and config files agree on this string; out-of-range values fall
/// back to `"unknown"` so logging never crashes on a corrupt cast.
[[nodiscard]] std::string_view to_string_view(Capability) noexcept;

/// Inverse of `to_string_view`. Returns `std::nullopt` when `text` does not
/// name a known capability (including the `"unknown"` fallback sentinel).
/// `noexcept` so it can be called from any context.
[[nodiscard]] std::optional<Capability> parse_capability(std::string_view text) noexcept;

/// Read-only view over every enumerator in declaration order. Useful for
/// schema generation, the future `--explain-rules` CLI, and tests that want
/// to iterate the universe without relying on cast arithmetic.
[[nodiscard]] std::span<const Capability> kAllCapabilities() noexcept;

}  // namespace orangutan::core

template <>
struct std::formatter<orangutan::core::Capability> : std::formatter<std::string_view> {
  template <class FormatContext>
  auto format(orangutan::core::Capability c, FormatContext& ctx) const {
    return std::formatter<std::string_view>::format(orangutan::core::to_string_view(c), ctx);
  }
};
