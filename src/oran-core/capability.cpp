// src/oran-core/capability.cpp — stable string mapping for `core::Capability`.

#include <oran/core/capability.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace orangutan::core {

namespace {

constexpr std::array<std::string_view, 19> kCapabilityNames{
    // file system
    "read_file",
    "write_file",
    "edit_file",
    "delete_path",
    // network
    "egress_http",
    "egress_websocket",
    // process
    "spawn_subprocess",
    "signal_subprocess",
    // memory
    "read_memory",
    "write_memory",
    // orchestration
    "spawn_agent",
    "send_message_intra_team",
    "send_message_inter_team",
    // automation
    "schedule_job",
    "modify_job",
    "run_job_now",
    // skills
    "invoke_skill",
    // misc
    "external_mcp",
    "runtime_loader",
};

constexpr std::array<Capability, kCapabilityNames.size()> kCapabilities{
    Capability::read_file,
    Capability::write_file,
    Capability::edit_file,
    Capability::delete_path,
    Capability::egress_http,
    Capability::egress_websocket,
    Capability::spawn_subprocess,
    Capability::signal_subprocess,
    Capability::read_memory,
    Capability::write_memory,
    Capability::spawn_agent,
    Capability::send_message_intra_team,
    Capability::send_message_inter_team,
    Capability::schedule_job,
    Capability::modify_job,
    Capability::run_job_now,
    Capability::invoke_skill,
    Capability::external_mcp,
    Capability::runtime_loader,
};

}  // namespace

std::string_view to_string_view(Capability c) noexcept {
  const auto idx = static_cast<std::size_t>(c);
  if (idx < kCapabilityNames.size()) {
    return kCapabilityNames[idx];
  }
  return "unknown";
}

std::optional<Capability> parse_capability(std::string_view text) noexcept {
  for (std::size_t i = 0; i < kCapabilityNames.size(); ++i) {
    if (text == kCapabilityNames[i]) {
      return kCapabilities[i];
    }
  }
  return std::nullopt;
}

std::span<const Capability> kAllCapabilities() noexcept {
  return std::span<const Capability>{kCapabilities};
}

}  // namespace orangutan::core
