#include <array>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <oran/async.hpp>
#include <oran/permission.hpp>
#include <oran/tool.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace permission = orangutan::permission;
namespace tool = orangutan::tool;

TEST_CASE("AgentRun rejects inputs outside the configured task contract", "[unit][tool][collaboration]") {
  std::string input;
  SECTION("unknown agent") {
    input = R"({"agent":"stranger","prompt":"inspect"})";
  }
  SECTION("authority override") {
    input = R"({"agent":"worker","prompt":"inspect","mode":"permissive"})";
  }
  SECTION("scope override") {
    input = R"({"agent":"worker","prompt":"inspect","scope_key":"elsewhere"})";
  }
  SECTION("session reuse") {
    input = R"({"agent":"worker","prompt":"inspect","session_id":"parent"})";
  }
  SECTION("missing prompt") {
    input = R"({"agent":"worker"})";
  }
  SECTION("empty prompt") {
    input = R"({"agent":"worker","prompt":""})";
  }
  SECTION("oversized prompt") {
    input = nlohmann::json{{"agent", "worker"}, {"prompt", std::string(16385, 'x')}}.dump();
  }
  orangutan::tests::run_async([&input](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    const auto names = std::array{std::string{"worker"}};
    REQUIRE(tool::register_agent_run(registry, names));
    permission::NullAuditSink audit;
    auto context = tool::DispatchContext::for_now(io.get_executor(), {}, audit);
    context.mode = permission::Mode::permissive;
    bool executed = false;
    context.agent_run = [&executed](tool::AgentRunRequest,
                                    tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
      executed = true;
      co_return tool::Output::text_only("child result");
    };

    auto result = co_await registry.dispatch(tool::AGENT_RUN_NAME, input, context);

    REQUIRE_FALSE(executed);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  });
}

TEST_CASE("AgentRun advertises configured names and delivers the authorized task", "[unit][tool][collaboration]") {
  orangutan::tests::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    const auto names = std::array{std::string{"worker"}, std::string{"reviewer"}};
    REQUIRE(tool::register_agent_run(registry, names));
    const auto* definition = registry.find(tool::AGENT_RUN_NAME);
    REQUIRE(definition != nullptr);
    const auto schema = nlohmann::json::parse(definition->input_schema_json);
    REQUIRE(schema["properties"]["agent"]["enum"] == nlohmann::json::array({"worker", "reviewer"}));
    REQUIRE(definition->required_capabilities == std::vector{core::Capability::spawn_agent});
    permission::RecordingAuditSink audit;
    auto context = tool::DispatchContext::for_now(io.get_executor(), {}, audit, "scope", "parent", "owner");
    context.mode = permission::Mode::permissive;
    tool::AgentRunRequest received;
    context.agent_run = [&received](tool::AgentRunRequest request,
                                    tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
      received = std::move(request);
      co_return tool::Output::text_only("review complete");
    };
    auto snapshot = tool::DispatchContext::for_now(context, false);

    auto result =
        co_await registry.dispatch(tool::AGENT_RUN_NAME, R"({"agent":"reviewer","prompt":"check changes"})", snapshot);

    REQUIRE(result.has_value());
    REQUIRE(result->text == "review complete");
    REQUIRE(received.agent == "reviewer");
    REQUIRE(received.prompt == "check changes");
    REQUIRE(audit.events().size() == 1);
    REQUIRE(audit.events()[0].tool_name == "AgentRun");
    REQUIRE(audit.events()[0].identity == "owner");
    REQUIRE(audit.events()[0].outcome == permission::AuditOutcome::allow);
  });
}
