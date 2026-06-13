// src/oran-bootstrap/channel_ingress.cpp - config-authored channel registration and routing.

#include <oran/bootstrap/channel_ingress.hpp>

#include <cstdlib>
#include <expected>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include <oran/channel/manager.hpp>
#include <oran/config.hpp>
#include <oran/core/error.hpp>

#if defined(ORAN_ENABLE_CHANNEL_QQ)
#include <oran/channel-qq.hpp>
#include <oran/http/client.hpp>
#endif

namespace orangutan::bootstrap {

namespace {

#if defined(ORAN_ENABLE_CHANNEL_QQ)

[[nodiscard]] core::Error
qq_credential_error(std::string message, const config::ChannelConfig& entry, std::string field) {
  return core::Error{core::ErrorKind::auth, std::move(message)}
      .with("channel_id", entry.id)
      .with("field", std::move(field));
}

[[nodiscard]] core::Result<std::string>
read_qq_secret_env(const config::ChannelConfig& entry, const std::string& env_name, std::string field) {
  if (env_name.empty()) {
    return std::unexpected(core::Error::config("qq channel credential env name must be non-empty")
                               .with("channel_id", entry.id)
                               .with("field", field));
  }
  const auto* value = std::getenv(env_name.c_str());
  if (value == nullptr) {
    return std::unexpected(qq_credential_error("qq credential environment variable is not set", entry, std::move(field))
                               .with("env", env_name));
  }
  auto secret = std::string{value};
  if (secret.empty()) {
    return std::unexpected(qq_credential_error("qq credential environment variable is empty", entry, std::move(field))
                               .with("env", env_name));
  }
  return secret;
}

class BootstrapQqChannel final : public channel::Channel {
public:
  BootstrapQqChannel(asio::any_io_executor executor,
                     channel::qq::Credentials credentials,
                     channel::qq::TokenStoreOptions token_options,
                     channel::qq::ApiClientOptions api_options,
                     channel::qq::GatewayTransportOptions transport_options,
                     channel::qq::QqChannelOptions channel_options)
      : http_client_{std::move(executor)}, token_store_{http_client_, std::move(credentials), std::move(token_options)},
        api_client_{http_client_, token_store_, std::move(api_options)},
        channel_{channel::qq::GatewayTransport{token_store_, std::move(transport_options)},
                 api_client_,
                 std::move(channel_options)} {}

  [[nodiscard]] std::string_view id() const noexcept override {
    return channel_.id();
  }

  [[nodiscard]] std::string_view kind() const noexcept override {
    return channel_.kind();
  }

  [[nodiscard]] channel::Capabilities capabilities() const noexcept override {
    return channel_.capabilities();
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> start() override {
    co_return co_await channel_.start();
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> stop() override {
    co_return co_await channel_.stop();
  }

  [[nodiscard]] async::Awaitable<core::Result<channel::InboundMessage>> next_message() override {
    co_return co_await channel_.next_message();
  }

  [[nodiscard]] async::Awaitable<core::Result<channel::DeliveryReceipt>>
  send(channel::OutboundMessage message) override {
    co_return co_await channel_.send(std::move(message));
  }

private:
  http::Client http_client_;
  channel::qq::TokenStore token_store_;
  channel::qq::ApiClient api_client_;
  channel::qq::QqChannel channel_;
};

[[nodiscard]] core::Result<std::unique_ptr<channel::Channel>> make_qq_channel(asio::any_io_executor executor,
                                                                              const config::ChannelConfig& entry) {
  auto app_id = read_qq_secret_env(entry, entry.qq_app_id_env, "qq_app_id_env");
  if (!app_id) {
    return std::unexpected(std::move(app_id).error());
  }
  auto client_secret = read_qq_secret_env(entry, entry.qq_client_secret_env, "qq_client_secret_env");
  if (!client_secret) {
    return std::unexpected(std::move(client_secret).error());
  }
  if (entry.qq_gateway_url.empty()) {
    return std::unexpected(
        core::Error::config("qq channel qq_gateway_url must be non-empty").with("channel_id", entry.id));
  }

  auto token_options = channel::qq::TokenStoreOptions{};
  if (!entry.qq_token_url.empty()) {
    token_options.token_url = entry.qq_token_url;
  }
  auto api_options = channel::qq::ApiClientOptions{};
  if (!entry.qq_api_base_url.empty()) {
    api_options.base_url = entry.qq_api_base_url;
  }
  auto transport_options = channel::qq::GatewayTransportOptions{};
  transport_options.gateway_url = entry.qq_gateway_url;
  auto channel_options = channel::qq::QqChannelOptions{};
  channel_options.id = entry.id;

  return std::make_unique<BootstrapQqChannel>(std::move(executor),
                                              channel::qq::Credentials{
                                                  .app_id = std::move(*app_id),
                                                  .client_secret = std::move(*client_secret),
                                              },
                                              std::move(token_options),
                                              std::move(api_options),
                                              std::move(transport_options),
                                              std::move(channel_options));
}

#endif

}  // namespace

core::Result<ChannelRegistrationReport> register_configured_channels(channel::ChannelManager& manager,
                                                                     asio::any_io_executor executor,
                                                                     const config::Config& cfg) {
  if (!executor) {
    return std::unexpected(core::Error::invalid_argument("channel registration requires an executor"));
  }

  auto report = ChannelRegistrationReport{};
  for (const auto& entry : cfg.channels()) {
    if (entry.kind == "qq") {
#if defined(ORAN_ENABLE_CHANNEL_QQ)
      auto adapter = make_qq_channel(executor, entry);
      if (!adapter) {
        return std::unexpected(std::move(adapter).error());
      }
      if (auto registered = manager.register_adapter(std::move(*adapter)); !registered) {
        return std::unexpected(std::move(registered).error());
      }
      ++report.registered_count;
#else
      report.skipped.push_back(SkippedChannelConfig{.id = entry.id, .kind = entry.kind});
#endif
      continue;
    }

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
