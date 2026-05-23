// tests/agent/test_loop.cpp — first fake-provider-backed Loop coverage.
//
// These tests intentionally cover only the first spec-0017 loop slice:
// prompt/request construction, text-only terminal responses, and error
// boundaries. Tool dispatch is tested later when `Loop` owns the
// tool_result-and-retry iteration path.

#include <oran/agent.hpp>

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/config.hpp>
#include <oran/core/content.hpp>
#include <oran/core/error.hpp>
#include <oran/core/message.hpp>
#include <oran/core/stop_reason.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/provider.hpp>

#include "../test-helpers/run_async.hpp"

namespace agent = orangutan::agent;
namespace async = orangutan::async;
namespace config = orangutan::config;
namespace core = orangutan::core;
namespace provider = orangutan::provider;
namespace test = orangutan::tests;

namespace {

provider::Route default_route(provider::PromptCacheOptions cache = {}) {
  return provider::Route{
      .primary =
          provider::ModelTarget{
              .profile = "fake",
              .model = "fake-1",
              .protocol = provider::ProtocolKind::anthropic_messages,
              .thinking_budget = std::nullopt,
              .cache = cache,
          },
      .fallbacks = {},
  };
}

core::ToolDef tool_def(std::string name, std::string description, bool deferred = false) {
  return core::ToolDef{
      .name = std::move(name),
      .description = std::move(description),
      .input_schema_json = R"({"type":"object","properties":{},"additionalProperties":false})",
      .required_capabilities = {},
      .deferred = deferred,
      .category = "test",
  };
}

std::vector<core::ToolDef> loop_catalog() {
  return {
      tool_def("memory.recall", "Recall memory", true),
      tool_def("file.read", "Read a file"),
      tool_def("custom.non_default", "A registered tool outside the default prompt set"),
  };
}

agent::RunTurnInputs base_inputs(const std::vector<core::ToolDef>& catalog, const std::vector<core::Message>& tail) {
  return agent::RunTurnInputs{
      .system_preamble = "system: deterministic test preamble",
      .tool_catalog = catalog,
      .active_tools = config::PromptActiveToolsConfig{},
      .promoted_tools = {},
      .skills_catalog = "skills: none",
      .memory_framing = "memory: none",
      .per_agent_overlay = "overlay: coder",
      .conversation_tail = tail,
      .tool_choice = std::string{"auto"},
      .max_tokens = 512,
      .thinking_budget = std::nullopt,
      .retry = provider::RetryPolicy{},
      .stream = true,
  };
}

/// RecordingProvider is a narrow white-box fixture for Loop's request mapping.
/// FakeProvider proves the scripted provider contract; this fixture proves the
/// loop sends the exact domain request future real adapters will consume.
class RecordingProvider final : public provider::System {
public:
  explicit RecordingProvider(provider::Response response) : response_{std::move(response)} {}

  [[nodiscard]] async::Awaitable<core::Result<provider::Response>>
  send(provider::Request request, provider::Route route, provider::EventSink* sink = nullptr) const override {
    request_ = std::move(request);
    route_ = std::move(route);
    ++calls_;
    if (sink != nullptr) {
      sink->on_done(response_.stop_reason);
    }
    co_return response_;
  }

  [[nodiscard]] const std::optional<provider::Request>& request() const noexcept {
    return request_;
  }

  [[nodiscard]] const std::optional<provider::Route>& route() const noexcept {
    return route_;
  }

  [[nodiscard]] std::size_t calls() const noexcept {
    return calls_;
  }

private:
  provider::Response response_;
  mutable std::optional<provider::Request> request_;
  mutable std::optional<provider::Route> route_;
  mutable std::size_t calls_{0};
};

}  // namespace

TEST_CASE("Loop returns text from a single fake-provider end_turn", "[unit][agent][loop]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response =
            provider::Response{
                .blocks = {core::TextContent{.text = "done"}},
                .stop_reason = core::StopReason::end_turn,
                .usage = provider::Usage{.input_tokens = 7,
                                         .output_tokens = 2,
                                         .cache_creation_tokens = 0,
                                         .cache_read_tokens = 0,
                                         .cost_estimate = std::nullopt},
                .model_used = std::string{"fake-1"},
            },
        .deltas = {},
        .error = std::nullopt,
        .latency = {},
    });
    provider::FakeProvider fake{std::move(plan)};
    agent::Loop loop{fake, default_route()};

    const auto catalog = loop_catalog();
    const std::vector<core::Message> tail{core::Message::user_text("hello")};
    auto result = co_await loop.run_turn(base_inputs(catalog, tail));

    REQUIRE(result.has_value());
    REQUIRE(result->text == "done");
    REQUIRE(result->stop_reason == core::StopReason::end_turn);
    REQUIRE(result->usage.input_tokens == 7);
    REQUIRE(result->usage.output_tokens == 2);
    REQUIRE(result->model_used == std::string{"fake-1"});
    REQUIRE(result->iterations == 1);
    REQUIRE(result->assistant_blocks.size() == 1);
    REQUIRE(result->rendered_prompt.sections.size() == 7);
    REQUIRE(result->cache_hints.has_value());
    REQUIRE(result->cache_hints->prefix_sections.size() == 6);
    REQUIRE(fake.turns_consumed() == 1);
  });
}

TEST_CASE("Loop maps prompt, messages, active tools, and cache hints into the provider request",
          "[unit][agent][loop]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    RecordingProvider provider{provider::Response{
        .blocks = {core::TextContent{.text = "mapped"}},
        .stop_reason = core::StopReason::end_turn,
        .usage = {},
        .model_used = std::nullopt,
    }};
    agent::Loop loop{provider, default_route(provider::PromptCacheOptions{.enabled = true, .min_prefix_bytes = 1})};

    const auto catalog = loop_catalog();
    const std::vector<std::string> promoted{"memory.recall"};
    const std::vector<core::Message> tail{core::Message::user_text("map this")};
    auto inputs = base_inputs(catalog, tail);
    inputs.promoted_tools = promoted;
    auto result = co_await loop.run_turn(inputs);

    REQUIRE(result.has_value());
    REQUIRE(provider.calls() == 1);
    REQUIRE(provider.request().has_value());

    const auto& request = *provider.request();
    REQUIRE(request.messages == tail);
    REQUIRE(request.stream);
    REQUIRE(request.tool_choice == std::string{"auto"});
    REQUIRE(request.max_tokens == 512);
    REQUIRE(request.system_prompt.has_value());
    REQUIRE(request.system_prompt->contains("system: deterministic test preamble"));
    REQUIRE(request.system_prompt->contains("Tool: file.read"));
    REQUIRE(request.system_prompt->contains("Tool: memory.recall"));
    REQUIRE_FALSE(request.system_prompt->contains("Tool: custom.non_default"));

    REQUIRE(request.tools.size() == 2);
    REQUIRE(request.tools[0].name == "file.read");
    REQUIRE(request.tools[1].name == "memory.recall");
    REQUIRE(request.cache.has_value());
    REQUIRE(request.cache->prefix_bytes == result->rendered_prompt.prefix_bytes);
    REQUIRE(provider.route().has_value());
    REQUIRE(provider.route()->primary.model == "fake-1");
  });
}

TEST_CASE("Loop forwards provider errors unchanged", "[unit][agent][loop]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response = std::nullopt,
        .deltas = {},
        .error = core::Error::network("upstream timeout"),
        .latency = {},
    });
    provider::FakeProvider fake{std::move(plan)};
    agent::Loop loop{fake, default_route()};

    const auto catalog = loop_catalog();
    const std::vector<core::Message> tail{core::Message::user_text("hello")};
    auto result = co_await loop.run_turn(base_inputs(catalog, tail));

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::network);
    REQUIRE(result.error().retryable());
  });
}

TEST_CASE("Loop rejects tool-use responses until the dispatch iteration slice lands", "[unit][agent][loop]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response =
            provider::Response{
                .blocks = {core::ToolUseContent{.id = "t1", .name = "file.read", .input_json = R"({"path":"x"})"}},
                .stop_reason = core::StopReason::tool_use,
                .usage = {},
                .model_used = std::nullopt,
            },
        .deltas = {},
        .error = std::nullopt,
        .latency = {},
    });
    provider::FakeProvider fake{std::move(plan)};
    agent::Loop loop{fake, default_route()};

    const auto catalog = loop_catalog();
    const std::vector<core::Message> tail{core::Message::user_text("read x")};
    auto result = co_await loop.run_turn(base_inputs(catalog, tail));

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::internal);
    REQUIRE(result.error().message() == "agent loop: response requires a later loop slice");
  });
}
