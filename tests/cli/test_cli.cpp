// tests/cli/test_cli.cpp — early CLI mode coverage.

#include <cerrno>
#include <expected>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <oran/async.hpp>
#include <oran/cli.hpp>
#include <oran/core/error.hpp>
#include <oran/core/stop_reason.hpp>
#include <oran/core/time.hpp>
#include <oran/hook.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace cli = orangutan::cli;
namespace core = orangutan::core;
namespace hook = orangutan::hook;
namespace test = orangutan::tests;

namespace {

class RecordingPromptRunner final : public cli::PromptRunner {
public:
  explicit RecordingPromptRunner(std::vector<core::Result<cli::PromptRunResult>> results = {})
      : results_{std::move(results)} {}

  async::Awaitable<core::Result<cli::PromptRunResult>> run_prompt(cli::PromptRunRequest request) override {
    requests.push_back(std::move(request));
    if (!results_.empty()) {
      auto result = std::move(results_.front());
      results_.erase(results_.begin());
      co_return result;
    }
    co_return cli::PromptRunResult{.text = "ok"};
  }

  std::vector<cli::PromptRunRequest> requests;

private:
  std::vector<core::Result<cli::PromptRunResult>> results_;
};

class ScopedStdin final {
public:
  explicit ScopedStdin(std::string_view input) {
    int pipe_fds[2] = {-1, -1};
    REQUIRE(::pipe(pipe_fds) == 0);

    saved_fd_ = ::dup(STDIN_FILENO);
    REQUIRE(saved_fd_ >= 0);

    auto remaining = input;
    while (!remaining.empty()) {
      const auto written = ::write(pipe_fds[1], remaining.data(), remaining.size());
      if (written < 0 && errno == EINTR) {
        continue;
      }
      REQUIRE(written > 0);
      remaining.remove_prefix(static_cast<std::size_t>(written));
    }

    REQUIRE(::close(pipe_fds[1]) == 0);
    pipe_fds[1] = -1;
    REQUIRE(::dup2(pipe_fds[0], STDIN_FILENO) >= 0);
    REQUIRE(::close(pipe_fds[0]) == 0);
  }

  ~ScopedStdin() {
    if (saved_fd_ >= 0) {
      (void)::dup2(saved_fd_, STDIN_FILENO);
      (void)::close(saved_fd_);
    }
  }

  ScopedStdin(const ScopedStdin&) = delete;
  ScopedStdin& operator=(const ScopedStdin&) = delete;
  ScopedStdin(ScopedStdin&&) = delete;
  ScopedStdin& operator=(ScopedStdin&&) = delete;

private:
  int saved_fd_{-1};
};

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

TEST_CASE("run handles scripted REPL slash commands without counting prompts", "[unit][cli]") {
  auto args = std::vector<std::string_view>{};

  SECTION("help is handled locally") {
    auto repl_lines = std::vector<std::string_view>{"/help", "continue"};
    auto result = cli::run(options(args, repl_lines));

    REQUIRE(result.has_value());
    REQUIRE(result->mode == cli::CliMode::repl);
    REQUIRE(result->prompts_processed == 1);
  }

  SECTION("exit stops later scripted prompts") {
    auto repl_lines = std::vector<std::string_view>{"first", "/exit", "ignored"};
    auto result = cli::run(options(args, repl_lines));

    REQUIRE(result.has_value());
    REQUIRE(result->mode == cli::CliMode::repl);
    REQUIRE(result->prompts_processed == 1);
  }

  SECTION("quit is an exit alias") {
    auto repl_lines = std::vector<std::string_view>{"  /quit  ", "ignored"};
    auto result = cli::run(options(args, repl_lines));

    REQUIRE(result.has_value());
    REQUIRE(result->mode == cli::CliMode::repl);
    REQUIRE(result->prompts_processed == 0);
  }
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

TEST_CASE("run_async delegates a single-shot prompt to the supplied runner", "[unit][cli][async]") {
  test::run_async([](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto args = std::vector<std::string_view>{"--prompt", "hello"};
    RecordingPromptRunner runner;

    auto result = co_await cli::run_async(options(args), &runner);

    REQUIRE(result.has_value());
    REQUIRE(result->mode == cli::CliMode::single_shot);
    REQUIRE(result->prompts_processed == 1);
    REQUIRE(result->exit_code == 0);
    REQUIRE(runner.requests.size() == 1);
    REQUIRE(runner.requests[0].prompt == "hello");
    REQUIRE(runner.requests[0].mode == cli::CliMode::single_shot);
    REQUIRE(runner.requests[0].prompt_index == 0);
  });
}

TEST_CASE("run_async delegates scripted REPL prompts in order", "[unit][cli][async]") {
  test::run_async([](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto args = std::vector<std::string_view>{};
    auto repl_lines = std::vector<std::string_view>{"first", "", "second"};
    RecordingPromptRunner runner;

    auto result = co_await cli::run_async(options(args, repl_lines), &runner);

    REQUIRE(result.has_value());
    REQUIRE(result->mode == cli::CliMode::repl);
    REQUIRE(result->prompts_processed == 2);
    REQUIRE(runner.requests.size() == 2);
    REQUIRE(runner.requests[0].prompt == "first");
    REQUIRE(runner.requests[0].mode == cli::CliMode::repl);
    REQUIRE(runner.requests[0].prompt_index == 0);
    REQUIRE(runner.requests[1].prompt == "second");
    REQUIRE(runner.requests[1].mode == cli::CliMode::repl);
    REQUIRE(runner.requests[1].prompt_index == 1);
  });
}

TEST_CASE("run_async handles scripted REPL slash commands before runner dispatch", "[unit][cli][async]") {
  test::run_async([](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto args = std::vector<std::string_view>{};
    auto repl_lines = std::vector<std::string_view>{"/help", "first", " /exit ", "ignored"};
    RecordingPromptRunner runner;

    auto result = co_await cli::run_async(options(args, repl_lines), &runner);

    REQUIRE(result.has_value());
    REQUIRE(result->mode == cli::CliMode::repl);
    REQUIRE(result->prompts_processed == 1);
    REQUIRE(runner.requests.size() == 1);
    REQUIRE(runner.requests[0].prompt == "first");
    REQUIRE(runner.requests[0].prompt_index == 0);
  });
}

TEST_CASE("run_async reads interactive REPL prompts until an empty line", "[unit][cli][async]") {
  test::run_async([](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto stdin_lines = ScopedStdin{"first\nsecond\n\nignored\n"};
    auto args = std::vector<std::string_view>{};
    auto cli_options = options(args);
    cli_options.interactive_repl = true;
    RecordingPromptRunner runner;

    auto result = co_await cli::run_async(cli_options, &runner);

    REQUIRE(result.has_value());
    REQUIRE(result->mode == cli::CliMode::repl);
    REQUIRE(result->prompts_processed == 2);
    REQUIRE(result->exit_code == 0);
    REQUIRE(runner.requests.size() == 2);
    REQUIRE(runner.requests[0].prompt == "first");
    REQUIRE(runner.requests[0].mode == cli::CliMode::repl);
    REQUIRE(runner.requests[0].prompt_index == 0);
    REQUIRE(runner.requests[1].prompt == "second");
    REQUIRE(runner.requests[1].mode == cli::CliMode::repl);
    REQUIRE(runner.requests[1].prompt_index == 1);
  });
}

TEST_CASE("run_async handles interactive REPL slash commands before runner dispatch", "[unit][cli][async]") {
  test::run_async([](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto stdin_lines = ScopedStdin{"/help\nfirst\n/quit\nignored\n"};
    auto args = std::vector<std::string_view>{};
    auto cli_options = options(args);
    cli_options.interactive_repl = true;
    RecordingPromptRunner runner;

    auto result = co_await cli::run_async(cli_options, &runner);

    REQUIRE(result.has_value());
    REQUIRE(result->mode == cli::CliMode::repl);
    REQUIRE(result->prompts_processed == 1);
    REQUIRE(runner.requests.size() == 1);
    REQUIRE(runner.requests[0].prompt == "first");
    REQUIRE(runner.requests[0].prompt_index == 0);
  });
}

TEST_CASE("run_async treats EOF after a partial interactive REPL line as a prompt", "[unit][cli][async]") {
  test::run_async([](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto stdin_lines = ScopedStdin{"partial"};
    auto args = std::vector<std::string_view>{};
    auto cli_options = options(args);
    cli_options.interactive_repl = true;
    RecordingPromptRunner runner;

    auto result = co_await cli::run_async(cli_options, &runner);

    REQUIRE(result.has_value());
    REQUIRE(result->mode == cli::CliMode::repl);
    REQUIRE(result->prompts_processed == 1);
    REQUIRE(runner.requests.size() == 1);
    REQUIRE(runner.requests[0].prompt == "partial");
    REQUIRE(runner.requests[0].prompt_index == 0);
  });
}

TEST_CASE("run_async ignores interactive REPL input when scripted prompts are supplied", "[unit][cli][async]") {
  test::run_async([](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto stdin_lines = ScopedStdin{"interactive\n\n"};
    auto args = std::vector<std::string_view>{};
    auto repl_lines = std::vector<std::string_view>{"scripted"};
    auto cli_options = options(args, repl_lines);
    cli_options.interactive_repl = true;
    RecordingPromptRunner runner;

    auto result = co_await cli::run_async(cli_options, &runner);

    REQUIRE(result.has_value());
    REQUIRE(result->mode == cli::CliMode::repl);
    REQUIRE(result->prompts_processed == 1);
    REQUIRE(runner.requests.size() == 1);
    REQUIRE(runner.requests[0].prompt == "scripted");
    REQUIRE(runner.requests[0].prompt_index == 0);
  });
}

TEST_CASE("run_async does not enter interactive REPL when only empty scripted prompts are supplied",
          "[unit][cli][async]") {
  test::run_async([](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto stdin_lines = ScopedStdin{"interactive\n\n"};
    auto args = std::vector<std::string_view>{};
    auto repl_lines = std::vector<std::string_view>{"", ""};
    auto cli_options = options(args, repl_lines);
    cli_options.interactive_repl = true;
    RecordingPromptRunner runner;

    auto result = co_await cli::run_async(cli_options, &runner);

    REQUIRE(result.has_value());
    REQUIRE(result->mode == cli::CliMode::repl);
    REQUIRE(result->prompts_processed == 0);
    REQUIRE(runner.requests.empty());
  });
}

TEST_CASE("run_async preserves deterministic shell behavior without a runner", "[unit][cli][async]") {
  test::run_async([](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto args = std::vector<std::string_view>{};
    auto repl_lines = std::vector<std::string_view>{"first", "", "second"};

    auto result = co_await cli::run_async(options(args, repl_lines));

    REQUIRE(result.has_value());
    REQUIRE(result->mode == cli::CliMode::repl);
    REQUIRE(result->prompts_processed == 2);
    REQUIRE(result->exit_code == 0);
  });
}

TEST_CASE("run_async does not enter interactive REPL without a runner", "[unit][cli][async]") {
  test::run_async([](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto stdin_lines = ScopedStdin{"would-block-without-gate\n"};
    auto args = std::vector<std::string_view>{};
    auto cli_options = options(args);
    cli_options.interactive_repl = true;

    auto result = co_await cli::run_async(cli_options);

    REQUIRE(result.has_value());
    REQUIRE(result->mode == cli::CliMode::repl);
    REQUIRE(result->prompts_processed == 0);
    REQUIRE(result->exit_code == 0);
  });
}

TEST_CASE("run_async propagates prompt runner errors", "[unit][cli][async]") {
  test::run_async([](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto args = std::vector<std::string_view>{"--prompt=fail"};
    auto failures = std::vector<core::Result<cli::PromptRunResult>>{
        std::unexpected(core::Error::internal("runner failed")),
    };
    RecordingPromptRunner runner{std::move(failures)};

    auto result = co_await cli::run_async(options(args), &runner);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::internal);
    REQUIRE(runner.requests.size() == 1);
  });
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

TEST_CASE("StreamingPromptSink renders answer-text deltas live", "[unit][cli][streaming]") {
  std::ostringstream out;
  cli::StreamingPromptSink sink{cli::StreamingPromptSinkOptions{.out = &out}};

  sink.on_text_delta("Hello");
  sink.on_text_delta(" world");
  sink.on_done(core::StopReason::end_turn);

  REQUIRE(out.str() == "Hello world\n");
  REQUIRE(sink.text_deltas_rendered() == 2);
  REQUIRE(sink.rendered_answer_text());
  REQUIRE(sink.tool_starts_rendered() == 0);
}

TEST_CASE("StreamingPromptSink announces tool calls on their own line", "[unit][cli][streaming]") {
  std::ostringstream out;
  cli::StreamingPromptSink sink{cli::StreamingPromptSinkOptions{.out = &out}};

  sink.on_tool_start("toolu_1", "get_weather");
  sink.on_done(core::StopReason::tool_use);

  // No answer text streamed, so `on_done` adds no trailing newline of its own;
  // the tool marker self-terminates and `rendered_answer_text()` stays false so
  // the runner still prints any assembled text.
  REQUIRE(out.str() == "[tool: get_weather]\n");
  REQUIRE(sink.tool_starts_rendered() == 1);
  REQUIRE_FALSE(sink.rendered_answer_text());
  REQUIRE(sink.text_deltas_rendered() == 0);
}

TEST_CASE("StreamingPromptSink streams thinking deltas when enabled", "[unit][cli][streaming]") {
  std::ostringstream out;
  cli::StreamingPromptSink sink{cli::StreamingPromptSinkOptions{.out = &out, .render_thinking = true}};

  sink.on_thinking_delta("rea");
  sink.on_thinking_delta("soning");
  sink.on_done(core::StopReason::end_turn);

  REQUIRE(out.str() == "reasoning\n");
  REQUIRE_FALSE(sink.rendered_answer_text());
}

TEST_CASE("StreamingPromptSink suppresses thinking deltas when disabled", "[unit][cli][streaming]") {
  std::ostringstream out;
  cli::StreamingPromptSink sink{cli::StreamingPromptSinkOptions{.out = &out, .render_thinking = false}};

  sink.on_thinking_delta("hidden reasoning");
  sink.on_text_delta("answer");
  sink.on_done(core::StopReason::end_turn);

  REQUIRE(out.str() == "answer\n");
  REQUIRE(sink.text_deltas_rendered() == 1);
  REQUIRE(sink.rendered_answer_text());
}
