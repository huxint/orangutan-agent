// tests/provider/test_adapter_factory.cpp - provider adapter factory coverage.

#include <oran/provider.hpp>

#include <algorithm>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/core/content.hpp>
#include <oran/core/error.hpp>

#include "../test-helpers/run_async.hpp"

namespace {

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

provider::AdapterCredentialBundle credential_bundle() {
  auto resolution = provider::RouteProfileResolution{
      .primary = profile_target("anthropic-main",
                                "claude-sonnet",
                                provider::ProtocolKind::anthropic_messages,
                                "anthropic",
                                "https://api.anthropic.com",
                                "ANTHROPIC_API_KEY"),
      .fallbacks = {profile_target("openai-main",
                                   "gpt-main",
                                   provider::ProtocolKind::openai_responses,
                                   "openai",
                                   "https://api.openai.com/v1",
                                   "OPENAI_API_KEY")},
  };
  auto plan = provider::make_adapter_construction_plan(resolution);
  REQUIRE(plan.has_value());

  return provider::AdapterCredentialBundle{
      .primary =
          provider::AdapterCredentialTarget{
              .target = plan->primary,
              .api_key = "anthropic-secret",
          },
      .fallbacks = {provider::AdapterCredentialTarget{
          .target = plan->fallbacks[0],
          .api_key = "openai-secret",
      }},
  };
}

provider::Request request() {
  auto request = provider::Request{};
  request.messages.push_back(core::Message{
      .role = core::Role::user,
      .blocks = {core::TextContent{.text = "hello"}},
      .created_at = {},
  });
  return request;
}

provider::Response response_for(std::string text) {
  return provider::Response{
      .blocks = {core::TextContent{.text = std::move(text)}},
      .stop_reason = core::StopReason::end_turn,
      .usage = {},
      .model_used = std::nullopt,
  };
}

std::optional<std::string_view> context_value(const core::Error& error, std::string_view key) {
  const auto it = std::ranges::find_if(error.context(), [&](const auto& entry) { return entry.first == key; });
  if (it == error.context().end()) {
    return std::nullopt;
  }
  return it->second;
}

class CapturingSystem final : public provider::System {
public:
  CapturingSystem(std::string label, provider::Response response)
      : label_{std::move(label)}, response_{std::move(response)} {}

  [[nodiscard]] async::Awaitable<core::Result<provider::Response>>
  send(provider::Request request, provider::Route route, provider::EventSink* sink = nullptr) const override {
    static_cast<void>(sink);
    requests.push_back(std::move(request));
    routes.push_back(std::move(route));
    co_return response_;
  }

  std::string label_;
  provider::Response response_;
  mutable std::vector<provider::Request> requests;
  mutable std::vector<provider::Route> routes;
};

class CapturingFactory final : public provider::ProtocolAdapterFactory {
public:
  explicit CapturingFactory(std::string label) : label_{std::move(label)} {}

  [[nodiscard]] core::Result<std::unique_ptr<provider::System>>
  create(provider::AdapterCredentialTarget target) const override {
    targets.push_back(target);
    auto system = std::make_unique<CapturingSystem>(label_, response_for(label_));
    systems.push_back(system.get());
    return system;
  }

  std::string label_;
  mutable std::vector<provider::AdapterCredentialTarget> targets;
  mutable std::vector<CapturingSystem*> systems;
};

class FailingFactory final : public provider::ProtocolAdapterFactory {
public:
  [[nodiscard]] core::Result<std::unique_ptr<provider::System>>
  create(provider::AdapterCredentialTarget target) const override {
    return std::unexpected(core::Error::config("factory failed").with("profile", target.target.profile.target.profile));
  }
};

class NullFactory final : public provider::ProtocolAdapterFactory {
public:
  [[nodiscard]] core::Result<std::unique_ptr<provider::System>>
  create(provider::AdapterCredentialTarget target) const override {
    static_cast<void>(target);
    return std::unique_ptr<provider::System>{};
  }
};

}  // namespace

TEST_CASE("adapter factory builds profile-routed provider system", "[unit][provider][adapter]") {
  auto credentials = credential_bundle();
  const auto expected_route = credentials.route();
  CapturingFactory anthropic{"anthropic-response"};
  CapturingFactory openai{"openai-response"};
  const auto factories = std::vector<provider::ProtocolAdapterFactoryBinding>{
      {.adapter_name = "anthropic_messages", .factory = &anthropic},
      {.adapter_name = "openai_responses", .factory = &openai},
  };

  auto system = provider::make_adapter_system(std::move(credentials), factories);

  REQUIRE(system.has_value());
  REQUIRE(anthropic.targets.size() == 1);
  REQUIRE(openai.targets.size() == 1);
  REQUIRE(anthropic.targets[0].target.profile.target.profile == "anthropic-main");
  REQUIRE(anthropic.targets[0].api_key == "anthropic-secret");
  REQUIRE(openai.targets[0].target.profile.target.profile == "openai-main");
  REQUIRE(openai.targets[0].api_key == "openai-secret");

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto primary =
        co_await (*system)->send(request(), provider::Route{.primary = expected_route.primary, .fallbacks = {}});
    REQUIRE(primary.has_value());
    REQUIRE(std::get<core::TextContent>(primary->blocks.front()).text == "anthropic-response");

    auto fallback =
        co_await (*system)->send(request(), provider::Route{.primary = expected_route.fallbacks[0], .fallbacks = {}});
    REQUIRE(fallback.has_value());
    REQUIRE(std::get<core::TextContent>(fallback->blocks.front()).text == "openai-response");
  });

  REQUIRE(anthropic.systems.size() == 1);
  REQUIRE(openai.systems.size() == 1);
  REQUIRE(anthropic.systems[0]->routes.size() == 1);
  REQUIRE(openai.systems[0]->routes.size() == 1);
  REQUIRE(anthropic.systems[0]->routes[0].primary.profile == "anthropic-main");
  REQUIRE(openai.systems[0]->routes[0].primary.profile == "openai-main");
  REQUIRE(anthropic.systems[0]->routes[0].fallbacks.empty());
  REQUIRE(openai.systems[0]->routes[0].fallbacks.empty());
}

TEST_CASE("adapter factory rejects missing protocol factory bindings", "[unit][provider][adapter]") {
  auto credentials = credential_bundle();
  CapturingFactory anthropic{"anthropic-response"};
  const auto factories = std::vector<provider::ProtocolAdapterFactoryBinding>{
      {.adapter_name = "anthropic_messages", .factory = &anthropic},
  };

  auto system = provider::make_adapter_system(std::move(credentials), factories);

  REQUIRE_FALSE(system.has_value());
  REQUIRE(system.error().kind() == core::ErrorKind::config);
  REQUIRE(context_value(system.error(), "role") == std::optional<std::string_view>{"fallback"});
  REQUIRE(context_value(system.error(), "profile") == std::optional<std::string_view>{"openai-main"});
  REQUIRE(context_value(system.error(), "adapter_name") == std::optional<std::string_view>{"openai_responses"});
}

TEST_CASE("adapter factory rejects invalid factory binding entries", "[unit][provider][adapter]") {
  auto credentials = credential_bundle();
  CapturingFactory anthropic{"anthropic-response"};
  const auto factories = std::vector<provider::ProtocolAdapterFactoryBinding>{
      {.adapter_name = "anthropic_messages", .factory = &anthropic},
      {.adapter_name = "openai_responses", .factory = nullptr},
  };

  auto system = provider::make_adapter_system(std::move(credentials), factories);

  REQUIRE_FALSE(system.has_value());
  REQUIRE(system.error().kind() == core::ErrorKind::config);
  REQUIRE(context_value(system.error(), "adapter_name") == std::optional<std::string_view>{"openai_responses"});
}

TEST_CASE("adapter factory rejects duplicate factory bindings", "[unit][provider][adapter]") {
  auto credentials = credential_bundle();
  CapturingFactory anthropic_a{"anthropic-a"};
  CapturingFactory anthropic_b{"anthropic-b"};
  CapturingFactory openai{"openai-response"};
  const auto factories = std::vector<provider::ProtocolAdapterFactoryBinding>{
      {.adapter_name = "anthropic_messages", .factory = &anthropic_a},
      {.adapter_name = "anthropic_messages", .factory = &anthropic_b},
      {.adapter_name = "openai_responses", .factory = &openai},
  };

  auto system = provider::make_adapter_system(std::move(credentials), factories);

  REQUIRE_FALSE(system.has_value());
  REQUIRE(system.error().kind() == core::ErrorKind::config);
  REQUIRE(context_value(system.error(), "adapter_name") == std::optional<std::string_view>{"anthropic_messages"});
  REQUIRE(anthropic_a.targets.empty());
  REQUIRE(anthropic_b.targets.empty());
}

TEST_CASE("adapter factory propagates concrete factory errors", "[unit][provider][adapter]") {
  auto credentials = credential_bundle();
  FailingFactory failing;
  CapturingFactory openai{"openai-response"};
  const auto factories = std::vector<provider::ProtocolAdapterFactoryBinding>{
      {.adapter_name = "anthropic_messages", .factory = &failing},
      {.adapter_name = "openai_responses", .factory = &openai},
  };

  auto system = provider::make_adapter_system(std::move(credentials), factories);

  REQUIRE_FALSE(system.has_value());
  REQUIRE(system.error().kind() == core::ErrorKind::config);
  REQUIRE(context_value(system.error(), "role") == std::optional<std::string_view>{"primary"});
  REQUIRE(context_value(system.error(), "profile") == std::optional<std::string_view>{"anthropic-main"});
}

TEST_CASE("adapter factory rejects null systems from concrete factories", "[unit][provider][adapter]") {
  auto credentials = credential_bundle();
  NullFactory null_factory;
  CapturingFactory openai{"openai-response"};
  const auto factories = std::vector<provider::ProtocolAdapterFactoryBinding>{
      {.adapter_name = "anthropic_messages", .factory = &null_factory},
      {.adapter_name = "openai_responses", .factory = &openai},
  };

  auto system = provider::make_adapter_system(std::move(credentials), factories);

  REQUIRE_FALSE(system.has_value());
  REQUIRE(system.error().kind() == core::ErrorKind::config);
  REQUIRE(context_value(system.error(), "role") == std::optional<std::string_view>{"primary"});
  REQUIRE(context_value(system.error(), "profile") == std::optional<std::string_view>{"anthropic-main"});
}

TEST_CASE("adapter factory rejects duplicate route profiles before fallback construction",
          "[unit][provider][adapter]") {
  auto credentials = credential_bundle();
  credentials.fallbacks[0].target.profile.target.profile = "anthropic-main";
  CapturingFactory anthropic{"anthropic-response"};
  CapturingFactory openai{"openai-response"};
  const auto factories = std::vector<provider::ProtocolAdapterFactoryBinding>{
      {.adapter_name = "anthropic_messages", .factory = &anthropic},
      {.adapter_name = "openai_responses", .factory = &openai},
  };

  auto system = provider::make_adapter_system(std::move(credentials), factories);

  REQUIRE_FALSE(system.has_value());
  REQUIRE(system.error().kind() == core::ErrorKind::config);
  REQUIRE(context_value(system.error(), "role") == std::optional<std::string_view>{"fallback"});
  REQUIRE(context_value(system.error(), "profile") == std::optional<std::string_view>{"anthropic-main"});
  REQUIRE(anthropic.targets.size() == 1);
  REQUIRE(openai.targets.empty());
}

TEST_CASE("adapter factory routes unknown profiles to config errors", "[unit][provider][adapter]") {
  auto credentials = credential_bundle();
  CapturingFactory anthropic{"anthropic-response"};
  CapturingFactory openai{"openai-response"};
  const auto factories = std::vector<provider::ProtocolAdapterFactoryBinding>{
      {.adapter_name = "anthropic_messages", .factory = &anthropic},
      {.adapter_name = "openai_responses", .factory = &openai},
  };
  auto system = provider::make_adapter_system(std::move(credentials), factories);
  REQUIRE(system.has_value());

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto result = co_await (*system)->send(request(),
                                           provider::Route{
                                               .primary =
                                                   provider::ModelTarget{
                                                       .profile = "unknown-profile",
                                                       .model = "unknown-model",
                                                       .protocol = provider::ProtocolKind::anthropic_messages,
                                                       .thinking_budget = std::nullopt,
                                                       .cache = std::nullopt,
                                                   },
                                               .fallbacks = {},
                                           });

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
    REQUIRE(context_value(result.error(), "profile") == std::optional<std::string_view>{"unknown-profile"});
    REQUIRE(context_value(result.error(), "model") == std::optional<std::string_view>{"unknown-model"});
  });
}

TEST_CASE("adapter factory routed system rejects multi-target routes", "[unit][provider][adapter]") {
  auto credentials = credential_bundle();
  const auto unexpected_multi_target_route = credentials.route();
  CapturingFactory anthropic{"anthropic-response"};
  CapturingFactory openai{"openai-response"};
  const auto factories = std::vector<provider::ProtocolAdapterFactoryBinding>{
      {.adapter_name = "anthropic_messages", .factory = &anthropic},
      {.adapter_name = "openai_responses", .factory = &openai},
  };
  auto system = provider::make_adapter_system(std::move(credentials), factories);
  REQUIRE(system.has_value());

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto result = co_await (*system)->send(request(), unexpected_multi_target_route);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
    REQUIRE(context_value(result.error(), "profile") == std::optional<std::string_view>{"anthropic-main"});
    REQUIRE(context_value(result.error(), "fallbacks") == std::optional<std::string_view>{"1"});
  });

  REQUIRE(anthropic.systems[0]->routes.empty());
  REQUIRE(openai.systems[0]->routes.empty());
}
