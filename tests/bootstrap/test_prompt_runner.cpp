// tests/bootstrap/test_prompt_runner.cpp - bootstrap AgentPromptRunner coverage.

#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
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
#include <oran/hook.hpp>
#include <oran/memory.hpp>
#include <oran/permission.hpp>
#include <oran/provider.hpp>
#include <oran/storage.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace bootstrap = orangutan::bootstrap;
namespace cli = orangutan::cli;
namespace config = orangutan::config;
namespace core = orangutan::core;
namespace hook = orangutan::hook;
namespace memory = orangutan::memory;
namespace permission = orangutan::permission;
namespace provider = orangutan::provider;
namespace test = orangutan::tests;

namespace {

struct ProviderHookCapture {
  hook::Event event;
  hook::Payload payload;
};

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
      .route_profile_used = std::nullopt,
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

bootstrap::RuntimeAssembly build_assembly(const std::filesystem::path& workspace,
                                          asio::io_context& io,
                                          bool audit_enabled,
                                          bool session_memory_enabled = false) {
  auto options = bootstrap::RuntimeAssemblyOptions{};
  options.audit_enabled = audit_enabled;
  options.session_memory_enabled = session_memory_enabled;
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

hook::InProcessSink provider_capture_sink(std::vector<ProviderHookCapture>& captures) {
  return hook::InProcessSink{
      "provider-capture",
      [&captures](hook::Event event, hook::Payload payload) -> async::Awaitable<core::Result<void>> {
        captures.push_back(ProviderHookCapture{.event = event, .payload = std::move(payload)});
        co_return core::Result<void>{};
      }};
}

class RecordingProvider final : public provider::System {
public:
  explicit RecordingProvider(std::vector<provider::Response> responses)
      : responses_{std::make_move_iterator(responses.begin()), std::make_move_iterator(responses.end())} {}

  [[nodiscard]] async::Awaitable<core::Result<provider::Response>>
  send(provider::Request request, provider::Route route, provider::EventSink* sink = nullptr) const override {
    static_cast<void>(route);
    {
      const std::lock_guard lock{mutex_};
      requests_.push_back(std::move(request));
      if (responses_.empty()) {
        co_return std::unexpected(core::Error::internal("recording provider plan exhausted"));
      }
    }

    provider::Response response;
    {
      const std::lock_guard lock{mutex_};
      response = std::move(responses_.front());
      responses_.pop_front();
    }
    if (sink != nullptr) {
      sink->on_done(response.stop_reason);
    }
    co_return response;
  }

  [[nodiscard]] std::vector<provider::Request> requests() const {
    const std::lock_guard lock{mutex_};
    return requests_;
  }

private:
  mutable std::mutex mutex_;
  mutable std::vector<provider::Request> requests_;
  mutable std::deque<provider::Response> responses_;
};

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

TEST_CASE("AgentPromptRunner persists successful turns through the session store",
          "[unit][bootstrap][prompt_runner][memory]") {
  TempDir temp{"oran-bootstrap-prompt-runner-session-persist"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false, true);
    REQUIRE(assembly.session_store() != nullptr);

    provider::FakeProvider fake{std::vector<provider::ScriptedTurn>{
        provider::ScriptedTurn{
            .response = text_response("stored"),
            .deltas = {},
            .error = std::nullopt,
            .latency = {},
        },
    }};

    auto options = base_runner_options(io, assembly, cfg, fake);
    core::TurnId session_id{};
    session_id.back() = std::byte{0x42};
    options.session_id = session_id;

    auto runner = bootstrap::AgentPromptRunner::create(std::move(options));
    REQUIRE(runner.has_value());
    auto result =
        co_await (*runner)->run_prompt(cli::PromptRunRequest{.prompt = "remember", .mode = cli::CliMode::single_shot});

    REQUIRE(result.has_value());
    REQUIRE(result->text == "stored");

    auto loaded =
        co_await assembly.session_store()->load(memory::session::SessionId{.value = "00000000000000000000000000000042"},
                                                memory::session::AgentKey{.value = "coder"});
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->size() == 2);
    REQUIRE((*loaded)[0].role == core::Role::user);
    REQUIRE((*loaded)[0].blocks == core::Message::user_text("remember").blocks);
    REQUIRE((*loaded)[1].role == core::Role::assistant);
    REQUIRE((*loaded)[1].blocks == core::Message::assistant_text("stored").blocks);
  });
}

TEST_CASE("AgentPromptRunner reloads persisted history for a new runner instance",
          "[unit][bootstrap][prompt_runner][memory]") {
  TempDir temp{"oran-bootstrap-prompt-runner-session-reload"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false, true);

    core::TurnId session_id{};
    session_id[0] = std::byte{0x12};
    session_id[15] = std::byte{0x34};

    {
      provider::FakeProvider fake{std::vector<provider::ScriptedTurn>{
          provider::ScriptedTurn{
              .response = text_response("first answer"),
              .deltas = {},
              .error = std::nullopt,
              .latency = {},
          },
      }};
      auto options = base_runner_options(io, assembly, cfg, fake);
      options.session_id = session_id;
      auto runner = bootstrap::AgentPromptRunner::create(std::move(options));
      REQUIRE(runner.has_value());
      auto first = co_await (*runner)->run_prompt(
          cli::PromptRunRequest{.prompt = "first prompt", .mode = cli::CliMode::single_shot});
      REQUIRE(first.has_value());
    }

    RecordingProvider recording{{text_response("second answer")}};
    auto options = base_runner_options(io, assembly, cfg, recording);
    options.session_id = session_id;
    auto runner = bootstrap::AgentPromptRunner::create(std::move(options));
    REQUIRE(runner.has_value());

    auto second = co_await (*runner)->run_prompt(
        cli::PromptRunRequest{.prompt = "second prompt", .mode = cli::CliMode::single_shot});

    REQUIRE(second.has_value());
    REQUIRE(second->text == "second answer");
    const auto requests = recording.requests();
    REQUIRE(requests.size() == 1);
    REQUIRE(requests[0].messages.size() == 3);
    REQUIRE(requests[0].messages[0].role == core::Role::user);
    REQUIRE(requests[0].messages[0].blocks == core::Message::user_text("first prompt").blocks);
    REQUIRE(requests[0].messages[1].role == core::Role::assistant);
    REQUIRE(requests[0].messages[1].blocks == core::Message::assistant_text("first answer").blocks);
    REQUIRE(requests[0].messages[2].role == core::Role::user);
    REQUIRE(requests[0].messages[2].blocks == core::Message::user_text("second prompt").blocks);

    auto loaded =
        co_await assembly.session_store()->load(memory::session::SessionId{.value = "12000000000000000000000000000034"},
                                                memory::session::AgentKey{.value = "coder"});
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->size() == 4);
    REQUIRE((*loaded)[3].blocks == core::Message::assistant_text("second answer").blocks);
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

TEST_CASE("AgentPromptRunner publishes provider hooks through RuntimeAssembly",
          "[unit][bootstrap][prompt_runner][provider][hooks]") {
  TempDir temp{"oran-bootstrap-prompt-runner-provider-hooks"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    std::vector<ProviderHookCapture> captures;
    auto sink = provider_capture_sink(captures);
    assembly.hook_bus().bind(sink, {hook::Event::provider_request, hook::Event::provider_response});

    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response = text_response("hooked"),
        .deltas = {},
        .error = std::nullopt,
        .latency = {},
    });
    provider::FakeProvider fake{std::move(plan)};

    auto runner = bootstrap::AgentPromptRunner::create(base_runner_options(io, assembly, cfg, fake));
    REQUIRE(runner.has_value());
    auto prompt = cli::PromptRunRequest{.prompt = "hooks", .mode = cli::CliMode::single_shot};
    auto result = co_await (*runner)->run_prompt(std::move(prompt));

    REQUIRE(result.has_value());
    REQUIRE(result->text == "hooked");
    REQUIRE(captures.size() == 2);
    REQUIRE(captures[0].event == hook::Event::provider_request);
    const auto* request = std::get_if<hook::ProviderRequestPayload>(&captures[0].payload);
    REQUIRE(request != nullptr);
    REQUIRE(request->who.scope_key == "scope-A");
    REQUIRE(request->who.agent_key == "coder");
    REQUIRE(request->who.identity == "operator-1");
    REQUIRE(request->origin == "cli");
    REQUIRE(request->route_profile == "fake");
    REQUIRE(request->route_model == "fake-1");
    REQUIRE(request->fallback_count == 0);
    REQUIRE(request->message_count == 1);

    REQUIRE(captures[1].event == hook::Event::provider_response);
    const auto* response = std::get_if<hook::ProviderResponsePayload>(&captures[1].payload);
    REQUIRE(response != nullptr);
    REQUIRE(response->who.agent_key == "coder");
    REQUIRE(response->served_profile == "fake");
    REQUIRE(response->served_model == "fake-1");
    REQUIRE(response->served_protocol == "anthropic_messages");
    REQUIRE(response->usage.input_tokens == 3);
    REQUIRE(response->usage.output_tokens == 2);
    REQUIRE(response->stop_reason == "end_turn");
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
                .route_profile_used = std::nullopt,
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
                .route_profile_used = std::nullopt,
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
