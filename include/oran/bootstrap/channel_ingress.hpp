// include/oran/bootstrap/channel_ingress.hpp - config-authored channel registration and routing.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <asio/any_io_executor.hpp>

#include <oran/bootstrap/channel_prompt_runner.hpp>
#include <oran/channel/dispatch.hpp>
#include <oran/channel/mock.hpp>
#include <oran/core/result.hpp>

namespace orangutan::config {
class Config;
}  // namespace orangutan::config

namespace orangutan::channel {
class ChannelManager;
}  // namespace orangutan::channel

namespace orangutan::bootstrap {

struct SkippedChannelConfig {
  std::string id;
  std::string kind;

  friend bool operator==(const SkippedChannelConfig&, const SkippedChannelConfig&) = default;
};

/// Non-owning handle to a registered mock adapter. The pointed-to adapter is
/// owned by the `ChannelManager` it was registered into and stays valid for
/// that manager's lifetime.
struct RegisteredMockChannel {
  std::string id;
  channel::MockChannel* mock{nullptr};
};

struct ChannelRegistrationReport {
  std::size_t registered_count{0};
  /// Config entries whose `kind` has no buildable adapter in this binary.
  /// Per `design-docs/channel-abstraction.md`, unknown kinds are skipped and
  /// reported so the caller can log one warning each.
  std::vector<SkippedChannelConfig> skipped{};
  /// Handles for registered `"mock"` adapters so callers (tests, loopback
  /// wiring) can push inbound messages and observe sends.
  std::vector<RegisteredMockChannel> mocks{};
};

/// Construct and register adapters for every `config.channels[]` entry.
/// `"mock"` is always constructible; `"qq"` is constructible only in builds
/// configured with `--channel_qq=y`. Other unknown or disabled platform kinds
/// are skipped and reported. Registration stays caller-owned: no adapter is
/// started and no receive loop is spawned.
[[nodiscard]] core::Result<ChannelRegistrationReport> register_configured_channels(channel::ChannelManager& manager,
                                                                                   asio::any_io_executor executor,
                                                                                   const config::Config& cfg);

/// Build one routed `ChannelPromptRunner` over `options.config->channels()`:
/// each channel id dispatches to its configured `agent_key` through a
/// per-agent `make_channel_agent_prompt_runner` bridge. `options.agent_key`
/// is ignored — the per-channel config value wins. Requests naming an
/// unconfigured channel id fail with `not_found`.
[[nodiscard]] core::Result<channel::ChannelPromptRunner>
make_routed_channel_prompt_runner(ChannelAgentPromptRunnerOptions options);

}  // namespace orangutan::bootstrap
