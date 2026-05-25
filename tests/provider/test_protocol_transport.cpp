// tests/provider/test_protocol_transport.cpp - provider protocol transport seam.

#include <oran/provider.hpp>

#include <algorithm>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <oran/async.hpp>
#include <oran/core/content.hpp>
#include <oran/core/error.hpp>

#include "../test-helpers/run_async.hpp"

namespace {

using json = ::nlohmann::ordered_json;

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace provider = orangutan::provider;
namespace test = orangutan::tests;

provider::ResolvedProfileTarget profile_target(std::string profile,
                                               std::string model,
                                               provider::ProtocolKind protocol,
                                               std::string provider_label,
                                               std::string base_url,
                                               std::string api_key_env) {
  return provider::ResolvedProfileTarget{
      .target =
          provider::ModelTarget{
              .profile = std::move(profile),
              .model = std::move(model),
              .protocol = protocol,
              .thinking_budget = std::nullopt,
              .cache = std::nullopt,
          },
      .provider = std::move(provider_label),
      .base_url = std::move(base_url),
      .api_key_env = std::move(api_key_env),
  };
}

provider::AdapterCredentialTarget credential_target(provider::ProtocolKind protocol) {
  const auto profile = protocol == provider::ProtocolKind::anthropic_messages
                           ? profile_target("anthropic-main",
                                            "claude-sonnet",
                                            protocol,
                                            "anthropic",
                                            "https://api.anthropic.com",
                                            "ANTHROPIC_API_KEY")
                           : profile_target("openai-main",
                                            "gpt-main",
                                            protocol,
                                            "openai",
                                            "https://api.openai.com/v1/",
                                            "OPENAI_API_KEY");
  auto plan = provider::make_adapter_construction_plan(provider::RouteProfileResolution{
      .primary = profile,
      .fallbacks = {},
  });
  REQUIRE(plan.has_value());
  return provider::AdapterCredentialTarget{
      .target = plan->primary,
      .api_key = protocol == provider::ProtocolKind::anthropic_messages ? "anthropic-secret" : "openai-secret",
  };
}

provider::AdapterCredentialBundle credential_bundle() {
  auto anthropic = credential_target(provider::ProtocolKind::anthropic_messages);
  auto openai = credential_target(provider::ProtocolKind::openai_responses);
  return provider::AdapterCredentialBundle{
      .primary = std::move(anthropic),
      .fallbacks = {std::move(openai)},
  };
}

provider::Request request() {
  auto request = provider::Request{};
  request.messages.push_back(core::Message{
      .role = core::Role::user,
      .blocks = {core::TextContent{.text = "hello"}},
      .created_at = {},
  });
  request.max_tokens = 64;
  request.stream = true;
  return request;
}

provider::ProtocolHttpResponse anthropic_response() {
  return provider::ProtocolHttpResponse{
      .status_code = 200,
      .headers = {},
      .body_json = R"json({
        "type": "message",
        "role": "assistant",
        "model": "claude-sonnet",
        "content": [{"type": "text", "text": "anthropic ok"}],
        "stop_reason": "end_turn",
        "usage": {"input_tokens": 4, "output_tokens": 2}
      })json",
  };
}

provider::ProtocolHttpResponse openai_response() {
  return provider::ProtocolHttpResponse{
      .status_code = 200,
      .headers = {},
      .body_json = R"json({
        "status": "completed",
        "model": "gpt-main",
        "output": [
          {
            "type": "message",
            "role": "assistant",
            "content": [{"type": "output_text", "text": "openai ok"}]
          }
        ],
        "usage": {"input_tokens": 5, "output_tokens": 3}
      })json",
  };
}

std::optional<std::string_view> context_value(const core::Error& error, std::string_view key) {
  const auto it = std::ranges::find_if(error.context(), [&](const auto& entry) { return entry.first == key; });
  if (it == error.context().end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<std::string_view> header_value(const provider::ProtocolHttpRequest& request, std::string_view name) {
  const auto it = std::ranges::find_if(request.headers, [&](const auto& header) { return header.name == name; });
  if (it == request.headers.end()) {
    return std::nullopt;
  }
  return it->value;
}

class RecordingTransport final : public provider::ProtocolTransport {
public:
  explicit RecordingTransport(std::vector<core::Result<provider::ProtocolHttpResponse>> responses)
      : responses_{std::move(responses)} {}

  [[nodiscard]] async::Awaitable<core::Result<provider::ProtocolHttpResponse>>
  send(provider::ProtocolHttpRequest request) const override {
    requests.push_back(std::move(request));
    const auto index = cursor++;
    if (index >= responses_.size()) {
      co_return std::unexpected(core::Error::internal("transport exhausted"));
    }
    const auto& response = responses_[index];
    if (response.has_value()) {
      co_return *response;
    }
    co_return std::unexpected(response.error());
  }

  std::vector<core::Result<provider::ProtocolHttpResponse>> responses_;
  mutable std::vector<provider::ProtocolHttpRequest> requests;
  mutable std::size_t cursor{0};
};

}  // namespace

TEST_CASE("protocol transport factory sends Anthropic Messages bodies", "[unit][provider][protocol]") {
  RecordingTransport transport{{anthropic_response()}};
  provider::ProtocolTransportAdapterFactory factory{
      transport,
      provider::ProtocolKind::anthropic_messages,
      provider::ProtocolTransportAdapterFactoryOptions{.anthropic_version = "2023-06-01"},
  };
  auto credentials = credential_target(provider::ProtocolKind::anthropic_messages);
  auto route = provider::Route{.primary = credentials.target.profile.target, .fallbacks = {}};
  auto system = factory.create(std::move(credentials));
  REQUIRE(system.has_value());

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto response = co_await (*system)->send(request(), route, nullptr);

    REQUIRE(response.has_value());
    REQUIRE(std::get<core::TextContent>(response->blocks.front()).text == "anthropic ok");
    REQUIRE(response->usage.input_tokens == 4);
  });

  REQUIRE(transport.requests.size() == 1);
  const auto& sent = transport.requests.front();
  REQUIRE(sent.method == "POST");
  REQUIRE(sent.url == "https://api.anthropic.com/v1/messages");
  REQUIRE(header_value(sent, "content-type") == std::optional<std::string_view>{"application/json"});
  REQUIRE(header_value(sent, "x-api-key") == std::optional<std::string_view>{"anthropic-secret"});
  REQUIRE(header_value(sent, "anthropic-version") == std::optional<std::string_view>{"2023-06-01"});
  const auto body = json::parse(sent.body_json);
  REQUIRE(body.at("model") == "claude-sonnet");
  REQUIRE(body.at("stream") == false);
}

TEST_CASE("protocol transport factory sends OpenAI Responses bodies", "[unit][provider][protocol]") {
  RecordingTransport transport{{openai_response()}};
  provider::ProtocolTransportAdapterFactory factory{transport, provider::ProtocolKind::openai_responses};
  auto credentials = credential_target(provider::ProtocolKind::openai_responses);
  auto route = provider::Route{.primary = credentials.target.profile.target, .fallbacks = {}};
  auto system = factory.create(std::move(credentials));
  REQUIRE(system.has_value());

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto response = co_await (*system)->send(request(), route, nullptr);

    REQUIRE(response.has_value());
    REQUIRE(std::get<core::TextContent>(response->blocks.front()).text == "openai ok");
    REQUIRE(response->usage.output_tokens == 3);
  });

  REQUIRE(transport.requests.size() == 1);
  const auto& sent = transport.requests.front();
  REQUIRE(sent.method == "POST");
  REQUIRE(sent.url == "https://api.openai.com/v1/responses");
  REQUIRE(header_value(sent, "authorization") == std::optional<std::string_view>{"Bearer openai-secret"});
  const auto body = json::parse(sent.body_json);
  REQUIRE(body.at("model") == "gpt-main");
  REQUIRE(body.at("stream") == false);
}

TEST_CASE("protocol transport factories work through profile-routed adapter system", "[unit][provider][protocol]") {
  RecordingTransport transport{{anthropic_response(), openai_response()}};
  provider::ProtocolTransportAdapterFactory anthropic{transport, provider::ProtocolKind::anthropic_messages};
  provider::ProtocolTransportAdapterFactory openai{transport, provider::ProtocolKind::openai_responses};
  auto credentials = credential_bundle();
  const auto route = credentials.route();
  const auto bindings = provider::protocol_transport_factory_bindings(anthropic, openai);
  auto system = provider::make_adapter_system(std::move(credentials), bindings);
  REQUIRE(system.has_value());

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto primary =
        co_await (*system)->send(request(), provider::Route{.primary = route.primary, .fallbacks = {}}, nullptr);
    auto fallback = co_await (*system)->send(request(),
                                             provider::Route{.primary = route.fallbacks.front(), .fallbacks = {}},
                                             nullptr);

    REQUIRE(primary.has_value());
    REQUIRE(fallback.has_value());
    REQUIRE(std::get<core::TextContent>(primary->blocks.front()).text == "anthropic ok");
    REQUIRE(std::get<core::TextContent>(fallback->blocks.front()).text == "openai ok");
  });

  REQUIRE(transport.requests.size() == 2);
}

TEST_CASE("protocol transport maps HTTP status errors without response bodies", "[unit][provider][protocol]") {
  SECTION("auth") {
    RecordingTransport transport{
        {provider::ProtocolHttpResponse{.status_code = 401, .headers = {}, .body_json = "{}"}}};
    provider::ProtocolTransportAdapterFactory factory{transport, provider::ProtocolKind::openai_responses};
    auto credentials = credential_target(provider::ProtocolKind::openai_responses);
    auto route = provider::Route{.primary = credentials.target.profile.target, .fallbacks = {}};
    auto system = factory.create(std::move(credentials));
    REQUIRE(system.has_value());

    test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
      auto response = co_await (*system)->send(request(), route, nullptr);

      REQUIRE_FALSE(response.has_value());
      REQUIRE(response.error().kind() == core::ErrorKind::auth);
      REQUIRE(context_value(response.error(), "http_status") == std::optional<std::string_view>{"401"});
    });
  }

  SECTION("rate limited") {
    RecordingTransport transport{
        {provider::ProtocolHttpResponse{.status_code = 429, .headers = {}, .body_json = "{}"}}};
    provider::ProtocolTransportAdapterFactory factory{transport, provider::ProtocolKind::openai_responses};
    auto credentials = credential_target(provider::ProtocolKind::openai_responses);
    auto route = provider::Route{.primary = credentials.target.profile.target, .fallbacks = {}};
    auto system = factory.create(std::move(credentials));
    REQUIRE(system.has_value());

    test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
      auto response = co_await (*system)->send(request(), route, nullptr);

      REQUIRE_FALSE(response.has_value());
      REQUIRE(response.error().kind() == core::ErrorKind::rate_limit);
      REQUIRE(context_value(response.error(), "http_status") == std::optional<std::string_view>{"429"});
    });
  }

  SECTION("upstream") {
    RecordingTransport transport{
        {provider::ProtocolHttpResponse{.status_code = 503, .headers = {}, .body_json = "{}"}}};
    provider::ProtocolTransportAdapterFactory factory{transport, provider::ProtocolKind::openai_responses};
    auto credentials = credential_target(provider::ProtocolKind::openai_responses);
    auto route = provider::Route{.primary = credentials.target.profile.target, .fallbacks = {}};
    auto system = factory.create(std::move(credentials));
    REQUIRE(system.has_value());

    test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
      auto response = co_await (*system)->send(request(), route, nullptr);

      REQUIRE_FALSE(response.has_value());
      REQUIRE(response.error().kind() == core::ErrorKind::upstream);
      REQUIRE(context_value(response.error(), "http_status") == std::optional<std::string_view>{"503"});
    });
  }
}

TEST_CASE("protocol transport factory rejects mismatched target protocols", "[unit][provider][protocol]") {
  SECTION("target protocol mismatch") {
    RecordingTransport transport{{anthropic_response()}};
    provider::ProtocolTransportAdapterFactory factory{transport, provider::ProtocolKind::anthropic_messages};
    auto credentials = credential_target(provider::ProtocolKind::openai_responses);

    auto system = factory.create(std::move(credentials));

    REQUIRE_FALSE(system.has_value());
    REQUIRE(system.error().kind() == core::ErrorKind::config);
    REQUIRE(context_value(system.error(), "factory_protocol") == std::optional<std::string_view>{"anthropic_messages"});
    REQUIRE(transport.requests.empty());
  }

  SECTION("unsupported factory protocol") {
    RecordingTransport transport{{anthropic_response()}};
    provider::ProtocolTransportAdapterFactory factory{transport, provider::ProtocolKind::openai_chat_completions};
    auto credentials = credential_target(provider::ProtocolKind::anthropic_messages);
    credentials.target.profile.target.protocol = provider::ProtocolKind::openai_chat_completions;
    credentials.target.adapter_name = "openai_chat_completions";

    auto system = factory.create(std::move(credentials));

    REQUIRE_FALSE(system.has_value());
    REQUIRE(system.error().kind() == core::ErrorKind::config);
    REQUIRE(context_value(system.error(), "factory_protocol") ==
            std::optional<std::string_view>{"openai_chat_completions"});
    REQUIRE(transport.requests.empty());
  }
}

TEST_CASE("protocol transport system rejects mismatched selected route models", "[unit][provider][protocol]") {
  RecordingTransport transport{{anthropic_response()}};
  provider::ProtocolTransportAdapterFactory factory{transport, provider::ProtocolKind::anthropic_messages};
  auto credentials = credential_target(provider::ProtocolKind::anthropic_messages);
  auto route = provider::Route{.primary = credentials.target.profile.target, .fallbacks = {}};
  route.primary.model = "other-model";
  auto system = factory.create(std::move(credentials));
  REQUIRE(system.has_value());

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto response = co_await (*system)->send(request(), route, nullptr);

    REQUIRE_FALSE(response.has_value());
    REQUIRE(response.error().kind() == core::ErrorKind::config);
    REQUIRE(context_value(response.error(), "route_model") == std::optional<std::string_view>{"other-model"});
  });

  REQUIRE(transport.requests.empty());
}
