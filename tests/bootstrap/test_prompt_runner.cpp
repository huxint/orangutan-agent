// tests/bootstrap/test_prompt_runner.cpp - bootstrap AgentPromptRunner coverage.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/bootstrap.hpp>
#include <oran/cli.hpp>
#include <oran/config.hpp>
#include <oran/core/content.hpp>
#include <oran/core/error.hpp>
#include <oran/core/stop_reason.hpp>
#include <oran/permission.hpp>
#include <oran/provider.hpp>
#include <oran/storage.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace bootstrap = orangutan::bootstrap;
namespace cli = orangutan::cli;
namespace config = orangutan::config;
namespace core = orangutan::core;
namespace permission = orangutan::permission;
namespace provider = orangutan::provider;
namespace test = orangutan::tests;

namespace {

class TempDir {
public:
  explicit TempDir(std::string name)
      : path_(std::filesystem::temp_directory_path() /
              (std::move(name) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
    std::filesystem::create_directories(path_);
  }

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  auto out = std::ofstream{path};
  out << contents;
}

provider::Route test_route() {
  return provider::Route{
      .primary =
          provider::ModelTarget{
              .profile = "fake",
              .model = "fake-1",
              .protocol = provider::ProtocolKind::anthropic_messages,
              .thinking_budget = std::nullopt,
              .cache = std::nullopt,
          },
      .fallbacks = {},
  };
}

provider::Response text_response(std::string text) {
  return provider::Response{
      .blocks = {core::TextContent{.text = std::move(text)}},
      .stop_reason = core::StopReason::end_turn,
      .usage = provider::Usage{.input_tokens = 3,
                               .output_tokens = 2,
                               .cache_creation_tokens = 0,
                               .cache_read_tokens = 0,
                               .cost_estimate = std::nullopt},
      .model_used = std::string{"fake-1"},
  };
}

cli::CliOptions cli_options(std::vector<std::string_view>& args) {
  return cli::CliOptions{
      .args = std::span<const std::string_view>{args},
      .quiet = true,
  };
}

config::Config parse_config(std::string_view json) {
  auto parsed = config::Config::parse(json);
  REQUIRE(parsed.has_value());
  return std::move(*parsed);
}

bootstrap::RuntimeAssembly
build_assembly(const std::filesystem::path& workspace, asio::io_context& io, bool audit_enabled) {
  auto options = bootstrap::RuntimeAssemblyOptions{};
  options.audit_enabled = audit_enabled;
  auto assembly = bootstrap::RuntimeAssembly::build(workspace.string(), io.get_executor(), std::move(options));
  REQUIRE(assembly.has_value());
  return std::move(*assembly);
}

bootstrap::AgentPromptRunnerOptions base_runner_options(asio::io_context& io,
                                                        bootstrap::RuntimeAssembly& assembly,
                                                        config::Config& cfg,
                                                        provider::System& provider_system) {
  auto options = bootstrap::AgentPromptRunnerOptions{};
  options.executor = io.get_executor();
  options.assembly = &assembly;
  options.config = &cfg;
  options.provider = &provider_system;
  options.route = test_route();
  options.scope_key = "scope-A";
  options.agent_key = "coder";
  options.identity = "operator-1";
  options.origin = "cli";
  options.quiet = true;
  return options;
}

}  // namespace

TEST_CASE("AgentPromptRunner rejects unknown permission overlays", "[unit][bootstrap][prompt_runner]") {
  TempDir temp{"oran-bootstrap-prompt-runner-bad-agent"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    provider::FakeProvider fake{std::vector<provider::ScriptedTurn>{}};
    auto options = base_runner_options(io, assembly, cfg, fake);
    options.permission_agent_name = "ghost";

    auto runner = bootstrap::AgentPromptRunner::create(std::move(options));

    REQUIRE_FALSE(runner.has_value());
    REQUIRE(runner.error().kind() == core::ErrorKind::not_found);
    co_return;
  });
}

TEST_CASE("AgentPromptRunner rejects an empty executor at create time", "[unit][bootstrap][prompt_runner]") {
  TempDir temp{"oran-bootstrap-prompt-runner-empty-executor"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    provider::FakeProvider fake{std::vector<provider::ScriptedTurn>{}};
    auto options = base_runner_options(io, assembly, cfg, fake);
    options.executor = asio::any_io_executor{};

    auto runner = bootstrap::AgentPromptRunner::create(std::move(options));

    REQUIRE_FALSE(runner.has_value());
    REQUIRE(runner.error().kind() == core::ErrorKind::invalid_argument);
    co_return;
  });
}

TEST_CASE("AgentPromptRunner drives CLI prompts through the agent loop and trace writer",
          "[unit][bootstrap][prompt_runner]") {
  TempDir temp{"oran-bootstrap-prompt-runner-trace"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, true);
    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response = text_response("runner ok"),
        .deltas = {},
        .error = std::nullopt,
        .latency = {},
    });
    provider::FakeProvider fake{std::move(plan)};

    auto runner = bootstrap::AgentPromptRunner::create(base_runner_options(io, assembly, cfg, fake));
    REQUIRE(runner.has_value());

    auto args = std::vector<std::string_view>{"--prompt", "hello"};
    auto result = co_await cli::run_async(cli_options(args), runner->get());

    REQUIRE(result.has_value());
    REQUIRE(result->mode == cli::CliMode::single_shot);
    REQUIRE(result->prompts_processed == 1);
    REQUIRE((*runner)->prompts_processed() == 1);
    REQUIRE(fake.turns_consumed() == 1);
    REQUIRE(assembly.trace_repository() != nullptr);
    auto count = co_await assembly.trace_repository()->count_turns();
    REQUIRE(count.has_value());
    REQUIRE(*count == 1);
  });
}

TEST_CASE("AgentPromptRunner uses provider execution retry before returning text",
          "[unit][bootstrap][prompt_runner][provider]") {
  TempDir temp{"oran-bootstrap-prompt-runner-retry"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response = std::nullopt,
        .deltas = {},
        .error = core::Error::network("transient"),
        .latency = {},
    });
    plan.push_back(provider::ScriptedTurn{
        .response = text_response("retried"),
        .deltas = {},
        .error = std::nullopt,
        .latency = {},
    });
    provider::FakeProvider fake{std::move(plan)};
    auto options = base_runner_options(io, assembly, cfg, fake);
    options.retry.max_attempts = 2;

    auto runner = bootstrap::AgentPromptRunner::create(std::move(options));
    REQUIRE(runner.has_value());
    auto prompt = cli::PromptRunRequest{.prompt = "retry", .mode = cli::CliMode::single_shot};
    auto result = co_await (*runner)->run_prompt(std::move(prompt));

    REQUIRE(result.has_value());
    REQUIRE(result->text == "retried");
    REQUIRE(fake.turns_consumed() == 2);
  });
}

TEST_CASE("AgentPromptRunner feeds tool.search results back into per-session state",
          "[unit][bootstrap][prompt_runner][session_state]") {
  TempDir temp{"oran-bootstrap-prompt-runner-observe"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response =
            provider::Response{
                .blocks = {core::ToolUseContent{
                    .id = "search-1",
                    .name = "tool.search",
                    .input_json = R"({"name":"file.read"})",
                }},
                .stop_reason = core::StopReason::tool_use,
                .usage = provider::Usage{.input_tokens = 1,
                                         .output_tokens = 1,
                                         .cache_creation_tokens = 0,
                                         .cache_read_tokens = 0,
                                         .cost_estimate = std::nullopt},
                .model_used = std::string{"fake-1"},
            },
        .deltas = {},
        .error = std::nullopt,
        .latency = {},
    });
    plan.push_back(provider::ScriptedTurn{
        .response = text_response("searched"),
        .deltas = {},
        .error = std::nullopt,
        .latency = {},
    });
    provider::FakeProvider fake{std::move(plan)};

    auto runner = bootstrap::AgentPromptRunner::create(base_runner_options(io, assembly, cfg, fake));
    REQUIRE(runner.has_value());
    auto prompt = cli::PromptRunRequest{.prompt = "search", .mode = cli::CliMode::single_shot};
    auto result = co_await (*runner)->run_prompt(std::move(prompt));

    REQUIRE(result.has_value());
    REQUIRE(result->text == "searched");
    REQUIRE(fake.turns_consumed() == 2);
    REQUIRE((*runner)->tool_search_observations_recorded() == 1);
  });
}

TEST_CASE("AgentPromptRunner binds the CLI approval sink for builtin tool dispatch",
          "[unit][bootstrap][prompt_runner][approval]") {
  TempDir temp{"oran-bootstrap-prompt-runner-approval"};
  write_file(temp.path() / "note.txt", "approved file\n");
  auto cfg = parse_config(R"json(
{
  "permissions": {
    "ask": [
      {"tool_pattern": "file.read"}
    ]
  }
}
)json");

  test::run_async([&temp, &cfg](asio::io_context& io) -> async::Awaitable<void> {
    auto assembly = build_assembly(temp.path(), io, false);
    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response =
            provider::Response{
                .blocks = {core::ToolUseContent{
                    .id = "read-1",
                    .name = "file.read",
                    .input_json = R"({"path":"note.txt"})",
                }},
                .stop_reason = core::StopReason::tool_use,
                .usage = provider::Usage{.input_tokens = 2,
                                         .output_tokens = 1,
                                         .cache_creation_tokens = 0,
                                         .cache_read_tokens = 0,
                                         .cost_estimate = std::nullopt},
                .model_used = std::string{"fake-1"},
            },
        .deltas = {},
        .error = std::nullopt,
        .latency = {},
    });
    plan.push_back(provider::ScriptedTurn{
        .response = text_response("approved final"),
        .deltas = {},
        .error = std::nullopt,
        .latency = {},
    });
    provider::FakeProvider fake{std::move(plan)};
    auto options = base_runner_options(io, assembly, cfg, fake);
    options.mode = permission::Mode::strict;
    options.approval_answers = {"yes"};

    auto runner = bootstrap::AgentPromptRunner::create(std::move(options));
    REQUIRE(runner.has_value());
    auto prompt = cli::PromptRunRequest{.prompt = "read note", .mode = cli::CliMode::single_shot};
    auto result = co_await (*runner)->run_prompt(std::move(prompt));

    REQUIRE(result.has_value());
    REQUIRE(result->text == "approved final");
    REQUIRE(fake.turns_consumed() == 2);
    REQUIRE((*runner)->approval_prompts_rendered() == 1);
  });
}

TEST_CASE("AgentPromptRunner streams answer deltas to the injected sink",
          "[unit][bootstrap][prompt_runner][streaming]") {
  TempDir temp{"oran-bootstrap-prompt-runner-stream"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response = std::nullopt,
        .deltas = {provider::TextDelta{.text = "hel"},
                   provider::TextDelta{.text = "lo"},
                   provider::StreamEnd{.stop_reason = core::StopReason::end_turn,
                                       .usage = std::nullopt,
                                       .model_used = std::string{"fake-1"}}},
        .error = std::nullopt,
        .latency = {},
    });
    provider::FakeProvider fake{std::move(plan)};

    std::ostringstream captured;
    auto options = base_runner_options(io, assembly, cfg, fake);
    options.quiet = false;
    options.stream_out = &captured;

    auto runner = bootstrap::AgentPromptRunner::create(std::move(options));
    REQUIRE(runner.has_value());
    auto prompt = cli::PromptRunRequest{.prompt = "hi", .mode = cli::CliMode::single_shot};
    auto result = co_await (*runner)->run_prompt(std::move(prompt));

    REQUIRE(result.has_value());
    REQUIRE(captured.str() == "hello\n");
    // The answer already appeared live, so the runner returns empty text and the
    // CLI does not print it a second time.
    REQUIRE(result->text.empty());
    REQUIRE(fake.turns_consumed() == 1);
  });
}

TEST_CASE("AgentPromptRunner keeps assembled text when nothing streamed",
          "[unit][bootstrap][prompt_runner][streaming]") {
  TempDir temp{"oran-bootstrap-prompt-runner-no-stream"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response = text_response("not streamed"),
        .deltas = {},
        .error = std::nullopt,
        .latency = {},
    });
    provider::FakeProvider fake{std::move(plan)};

    std::ostringstream captured;
    auto options = base_runner_options(io, assembly, cfg, fake);
    options.quiet = false;
    options.stream_out = &captured;

    auto runner = bootstrap::AgentPromptRunner::create(std::move(options));
    REQUIRE(runner.has_value());
    auto prompt = cli::PromptRunRequest{.prompt = "hi", .mode = cli::CliMode::single_shot};
    auto result = co_await (*runner)->run_prompt(std::move(prompt));

    REQUIRE(result.has_value());
    // No deltas fired, so nothing rendered live and the runner returns the
    // assembled text for the CLI to print itself.
    REQUIRE(captured.str().empty());
    REQUIRE(result->text == "not streamed");
  });
}
