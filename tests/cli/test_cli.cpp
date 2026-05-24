// tests/cli/test_cli.cpp — early CLI mode coverage.

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/cli.hpp>
#include <oran/core/error.hpp>
#include <oran/core/time.hpp>
#include <oran/hook.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace cli = orangutan::cli;
namespace core = orangutan::core;
namespace hook = orangutan::hook;
namespace test = orangutan::tests;

namespace {

cli::CliOptions options(std::vector<std::string_view>& args) {
  return cli::CliOptions{.args = std::span<const std::string_view>{args}, .quiet = true};
}

cli::CliOptions options(std::vector<std::string_view>& args, std::vector<std::string_view>& repl_lines) {
  return cli::CliOptions{
      .args = std::span<const std::string_view>{args},
      .repl_lines = std::span<const std::string_view>{repl_lines},
      .quiet = true,
  };
}

hook::PermissionAskRenderedPayload sample_ask_payload() {
  return hook::PermissionAskRenderedPayload{
      .tool_name = "file.write",
      .input_json = R"({"path":"notes.txt","content":"hello"})",
      .who = hook::Identity{.scope_key = "scope-A", .agent_key = "coder", .identity = "huxint"},
      .decision_reason = "rule #2 (ask: file.write)",
      .replay_max = 3,
      .approval_ttl = std::chrono::seconds{120},
      .requested_at = core::Time::epoch(),
  };
}

hook::ToolBeforePayload sample_before_payload() {
  return hook::ToolBeforePayload{
      .tool_name = "file.write",
      .input_json = R"({"path":"notes.txt","content":"hello"})",
      .who = hook::Identity{.scope_key = "scope-A", .agent_key = "coder", .identity = "huxint"},
      .started_at = core::Time::epoch(),
  };
}

}  // namespace

TEST_CASE("run selects REPL mode when no prompt is supplied", "[unit][cli]") {
  auto args = std::vector<std::string_view>{};

  auto result = cli::run(options(args));

  REQUIRE(result.has_value());
  REQUIRE(result->mode == cli::CliMode::repl);
  REQUIRE(result->prompts_processed == 0);
  REQUIRE(result->exit_code == 0);
}

TEST_CASE("run counts scripted REPL prompts", "[unit][cli]") {
  auto args = std::vector<std::string_view>{};
  auto repl_lines = std::vector<std::string_view>{"hello", "", "continue"};

  auto result = cli::run(options(args, repl_lines));

  REQUIRE(result.has_value());
  REQUIRE(result->mode == cli::CliMode::repl);
  REQUIRE(result->prompts_processed == 2);
}

TEST_CASE("run selects single-shot mode from prompt arguments", "[unit][cli]") {
  SECTION("--prompt value") {
    auto args = std::vector<std::string_view>{"--prompt", "What is 17 * 23?"};
    auto result = cli::run(options(args));

    REQUIRE(result.has_value());
    REQUIRE(result->mode == cli::CliMode::single_shot);
    REQUIRE(result->prompts_processed == 1);
  }

  SECTION("--prompt=value") {
    auto prompt_arg = std::string{"--prompt=Read README.md"};
    auto args = std::vector<std::string_view>{prompt_arg};
    auto result = cli::run(options(args));

    REQUIRE(result.has_value());
    REQUIRE(result->mode == cli::CliMode::single_shot);
    REQUIRE(result->prompts_processed == 1);
  }

  SECTION("xmake run separator") {
    auto args = std::vector<std::string_view>{"--", "--prompt", "hello"};
    auto result = cli::run(options(args));

    REQUIRE(result.has_value());
    REQUIRE(result->mode == cli::CliMode::single_shot);
    REQUIRE(result->prompts_processed == 1);
  }
}

TEST_CASE("run handles CLI help without dispatching a prompt", "[unit][cli]") {
  SECTION("help only") {
    auto args = std::vector<std::string_view>{"--help"};
    auto result = cli::run(options(args));

    REQUIRE(result.has_value());
    REQUIRE(result->mode == cli::CliMode::help);
    REQUIRE(result->prompts_processed == 0);
  }

  SECTION("help takes precedence over later args") {
    auto args = std::vector<std::string_view>{"--help", "--prompt", "ignored"};
    auto result = cli::run(options(args));

    REQUIRE(result.has_value());
    REQUIRE(result->mode == cli::CliMode::help);
    REQUIRE(result->prompts_processed == 0);
  }
}

TEST_CASE("run rejects invalid CLI arguments", "[unit][cli]") {
  SECTION("missing prompt value") {
    auto args = std::vector<std::string_view>{"--prompt"};
    auto result = cli::run(options(args));

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }

  SECTION("empty prompt value") {
    auto args = std::vector<std::string_view>{"--prompt", ""};
    auto result = cli::run(options(args));

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }

  SECTION("duplicate prompt") {
    auto args = std::vector<std::string_view>{"--prompt", "one", "--prompt", "two"};
    auto result = cli::run(options(args));

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }

  SECTION("unknown argument") {
    auto args = std::vector<std::string_view>{"--unknown"};
    auto result = cli::run(options(args));

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }
}

TEST_CASE("OperatorPromptSink approves permission ask prompts from scripted yes answers",
          "[unit][cli][hook][approval]") {
  test::run_async([](asio::io_context& /*io*/) -> async::Awaitable<void> {
    hook::Bus bus;
    cli::OperatorPromptSink sink{cli::OperatorPromptSinkOptions{
        .sink_id = "terminal-approval",
        .operator_identity = "fallback-operator",
        .scripted_answers = {" YES "},
        .quiet = true,
    }};
    bus.bind(sink, {hook::Event::permission_ask_rendered});

    auto decision = co_await bus.publish_blocking<hook::Event::permission_ask_rendered>(sample_ask_payload());

    REQUIRE(decision.has_value());
    REQUIRE(decision->kind == hook::HookDecisionKind::proceed);
    REQUIRE(decision->reason.empty());
    REQUIRE(decision->trace.size() == 1);
    REQUIRE(decision->trace[0].sink_id == "terminal-approval");
    REQUIRE(decision->trace[0].kind == hook::HookDecisionKind::proceed);
    REQUIRE(decision->trace[0].reason == "operator_approved:huxint");
    REQUIRE(sink.prompts_rendered() == 1);
    REQUIRE(sink.answers_consumed() == 1);
  });
}

TEST_CASE("OperatorPromptSink vetoes permission ask prompts from scripted no answers", "[unit][cli][hook][approval]") {
  test::run_async([](asio::io_context& /*io*/) -> async::Awaitable<void> {
    hook::Bus bus;
    cli::OperatorPromptSink sink{cli::OperatorPromptSinkOptions{
        .scripted_answers = {"n"},
        .quiet = true,
    }};
    bus.bind(sink, {hook::Event::permission_ask_rendered});

    auto decision = co_await bus.publish_blocking<hook::Event::permission_ask_rendered>(sample_ask_payload());

    REQUIRE(decision.has_value());
    REQUIRE(decision->kind == hook::HookDecisionKind::veto);
    REQUIRE(decision->reason == "operator_denied:huxint");
    REQUIRE(decision->trace.size() == 1);
    REQUIRE(decision->trace[0].kind == hook::HookDecisionKind::veto);
    REQUIRE(decision->trace[0].reason == "operator_denied:huxint");
  });
}

TEST_CASE("OperatorPromptSink falls back to configured identity when payload identity is empty",
          "[unit][cli][hook][approval]") {
  test::run_async([](asio::io_context& /*io*/) -> async::Awaitable<void> {
    hook::Bus bus;
    cli::OperatorPromptSink sink{cli::OperatorPromptSinkOptions{
        .operator_identity = "terminal-user",
        .scripted_answers = {"approve"},
        .quiet = true,
    }};
    bus.bind(sink, {hook::Event::permission_ask_rendered});
    auto payload = sample_ask_payload();
    payload.who.identity.clear();

    auto decision = co_await bus.publish_blocking<hook::Event::permission_ask_rendered>(payload);

    REQUIRE(decision.has_value());
    REQUIRE(decision->kind == hook::HookDecisionKind::proceed);
    REQUIRE(decision->reason.empty());
    REQUIRE(decision->trace.size() == 1);
    REQUIRE(decision->trace[0].reason == "operator_approved:terminal-user");
  });
}

TEST_CASE("OperatorPromptSink rejects invalid scripted answers as hook errors", "[unit][cli][hook][approval]") {
  test::run_async([](asio::io_context& /*io*/) -> async::Awaitable<void> {
    hook::Bus bus;
    cli::OperatorPromptSink sink{cli::OperatorPromptSinkOptions{
        .scripted_answers = {"maybe"},
        .quiet = true,
    }};
    bus.bind(sink, {hook::Event::permission_ask_rendered});

    auto decision = co_await bus.publish_blocking<hook::Event::permission_ask_rendered>(sample_ask_payload());

    REQUIRE(decision.has_value());
    REQUIRE(decision->kind == hook::HookDecisionKind::veto);
    REQUIRE(decision->reason.starts_with("hook_error: operator approval answer must be yes or no"));
    REQUIRE(decision->trace.size() == 1);
    REQUIRE(decision->trace[0].kind == hook::HookDecisionKind::veto);
    REQUIRE(decision->trace[0].reason == decision->reason);
    REQUIRE(sink.answers_consumed() == 1);
  });
}

TEST_CASE("OperatorPromptSink proceeds for non-ask blocking events", "[unit][cli][hook]") {
  test::run_async([](asio::io_context& /*io*/) -> async::Awaitable<void> {
    hook::Bus bus;
    cli::OperatorPromptSink sink{cli::OperatorPromptSinkOptions{
        .scripted_answers = {"no"},
        .quiet = true,
    }};
    bus.bind(sink, {hook::Event::tool_before});

    auto decision = co_await bus.publish_blocking<hook::Event::tool_before>(sample_before_payload());

    REQUIRE(decision.has_value());
    REQUIRE(decision->kind == hook::HookDecisionKind::proceed);
    REQUIRE(decision->trace.size() == 1);
    REQUIRE(decision->trace[0].kind == hook::HookDecisionKind::proceed);
    REQUIRE(sink.prompts_rendered() == 0);
    REQUIRE(sink.answers_consumed() == 0);
  });
}
