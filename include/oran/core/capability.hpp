// include/oran/core/capability.hpp — runtime capability vocabulary.
//
// `Capability` is the v2 mechanism that ties tools (`oran-tool`) to
// permission rules (`oran-permission`). Tools declare the capabilities they
// need via `ToolDef::requires` (future slice); permission rules may scope
// to a capability via `Rule::capability` (next slice).
//
// Stable string spelling and its inverse come from `core::enum_name(c)` /
// `core::parse_enum<Capability>(text)` in `enum_names.hpp`. The set of all
// enumerators is `core::enum_values<Capability>()`. The enum text matches the
// design doc verbatim (`docs/design-docs/tool-runtime.md`); breaking that
// mapping in a future change is a rule violation under
// `docs/rules/docs-in-sync.md`.

#pragma once

#include <cstdint>
#include <format>
#include <string_view>

#include <oran/core/enum_names.hpp>

namespace orangutan::core {

enum class Capability : std::uint8_t {
  // file system
  read_file,
  write_file,
  edit_file,
  delete_path,
  list_directory,
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
  deactivate_skill,
  // misc
  external_mcp,
  runtime_loader,
};

}  // namespace orangutan::core

template <>
struct std::formatter<orangutan::core::Capability> : std::formatter<std::string_view> {
  template <class FormatContext>
  auto format(orangutan::core::Capability c, FormatContext& ctx) const {
    return std::formatter<std::string_view>::format(orangutan::core::enum_name(c), ctx);
  }
};
