// src/oran-bootstrap/channel_ingress.cpp - config-authored channel registration and routing.

#include <oran/bootstrap/channel_ingress.hpp>

#include <expected>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include <oran/channel/manager.hpp>
#include <oran/config.hpp>
#include <oran/core/error.hpp>

namespace orangutan::bootstrap {

core::Result<ChannelRegistrationReport> register_configured_channels(channel::ChannelManager& manager,
                                                                     asio::any_io_executor executor,
                                                                     const config::Config& cfg) {
  if (!executor) {
    return std::unexpected(core::Error::invalid_argument("channel registration requires an executor"));
  }

  auto report = ChannelRegistrationReport{};
  for (const auto& entry : cfg.channels()) {
    if (entry.kind != "mock") {
      report.skipped.push_back(SkippedChannelConfig{.id = entry.id, .kind = entry.kind});
      continue;
    }

    auto adapter = std::make_unique<channel::MockChannel>(executor,
                                                          channel::MockChannelOptions{
                                                              .id = entry.id,
                                                              .kind = entry.kind,
                                                              .capabilities = {},
                                                              .inbound_capacity = entry.inbound_capacity,
                                                          });
    auto* mock = adapter.get();
    if (auto registered = manager.register_adapter(std::move(adapter)); !registered) {
      return std::unexpected(std::move(registered).error());
    }
    report.mocks.push_back(RegisteredMockChannel{.id = entry.id, .mock = mock});
    ++report.registered_count;
  }
  return report;
}

core::Result<channel::ChannelPromptRunner> make_routed_channel_prompt_runner(ChannelAgentPromptRunnerOptions options) {
  if (options.config == nullptr) {
    return std::unexpected(core::Error::invalid_argument("routed channel prompt runner requires a config"));
  }
  if (options.config->channels().empty()) {
    return std::unexpected(core::Error::invalid_argument("routed channel prompt runner requires configured channels"));
  }

  // One underlying bridge per distinct agent key; channels sharing an agent
  // share the bridge (session identity still differs per channel because the
  // bridge derives session ids from the request's channel id).
  auto bridges = std::map<std::string, std::shared_ptr<channel::ChannelPromptRunner>, std::less<>>{};
  auto routes = std::map<std::string, std::shared_ptr<channel::ChannelPromptRunner>, std::less<>>{};
  for (const auto& entry : options.config->channels()) {
    auto bridge_it = bridges.find(entry.agent_key);
    if (bridge_it == bridges.end()) {
      auto bridge_options = options;
      bridge_options.agent_key = entry.agent_key;
      auto bridge = make_channel_agent_prompt_runner(std::move(bridge_options));
      if (!bridge) {
        return std::unexpected(std::move(bridge).error());
      }
      bridge_it =
          bridges.emplace(entry.agent_key, std::make_shared<channel::ChannelPromptRunner>(std::move(*bridge))).first;
    }
    routes.emplace(entry.id, bridge_it->second);
  }

  return channel::ChannelPromptRunner{[routes = std::move(routes)](channel::ChannelPromptRunRequest request)
                                          -> async::Awaitable<core::Result<channel::ChannelPromptRunResult>> {
    const auto it = routes.find(request.channel_id);
    if (it == routes.end()) {
      co_return std::unexpected(
          core::Error::not_found("no agent route for channel").with("channel_id", request.channel_id));
    }
    co_return co_await (*it->second)(std::move(request));
  }};
}

}  // namespace orangutan::bootstrap
