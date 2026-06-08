// tests/bootstrap/test_automation_prompt_runner.cpp - bootstrap automation prompt bridge coverage.

#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/automation.hpp>
#include <oran/bootstrap.hpp>
#include <oran/cli.hpp>
#include <oran/config.hpp>
#include <oran/core/content.hpp>
#include <oran/core/error.hpp>
#include <oran/core/message.hpp>
#include <oran/core/stop_reason.hpp>
#include <oran/memory.hpp>
#include <oran/provider.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace automation = orangutan::automation;
namespace bootstrap = orangutan::bootstrap;
namespace cli = orangutan::cli;
namespace config = orangutan::config;
namespace core = orangutan::core;
namespace memory = orangutan::memory;
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
      .route_profile_used = std::nullopt,
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
                                          bool session_memory_enabled = false,
                                          bool longterm_memory_enabled = true) {
  auto options = bootstrap::RuntimeAssemblyOptions{};
  options.audit_enabled = audit_enabled;
  options.session_memory_enabled = session_memory_enabled;
  options.longterm_memory_enabled = longterm_memory_enabled;
  auto assembly = bootstrap::RuntimeAssembly::build(workspace.string(), io.get_executor(), std::move(options));
  REQUIRE(assembly.has_value());
  return std::move(*assembly);
}

template <typename Rep, typename Period>
[[nodiscard]] core::Time at(std::chrono::duration<Rep, Period> value) {
  return core::Time{core::Time::time_point{
      std::chrono::duration_cast<core::Time::clock::duration>(value),
  }};
}

[[nodiscard]] std::string automation_db_path(const TempDir& temp) {
  return (temp.path() / ".orangutan" / "automation.db").string();
}

std::string tool_result_output_in(const provider::Request& request, std::string_view tool_use_id) {
  for (const auto& message : request.messages) {
    for (const auto& block : message.blocks) {
      const auto* result = std::get_if<core::ToolResultContent>(&block);
      if (result != nullptr && result->tool_use_id == tool_use_id) {
        return result->output;
      }
    }
  }
  return {};
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

bootstrap::AutomationAgentPromptRunnerOptions base_bridge_options(asio::io_context& io,
                                                                  bootstrap::RuntimeAssembly& assembly,
                                                                  config::Config& cfg,
                                                                  provider::System& provider_system) {
  auto options = bootstrap::AutomationAgentPromptRunnerOptions{};
  options.executor = io.get_executor();
  options.assembly = &assembly;
  options.config = &cfg;
  options.provider = &provider_system;
  options.route = test_route();
  options.scope_key = "automation-scope";
  options.identity = "automation-worker";
  options.origin = "automation";
  options.max_tokens = 1024;
  return options;
}

}  // namespace

TEST_CASE("automation prompt bridge reloads persisted history for the same durable job",
          "[unit][bootstrap][automation_prompt_runner][memory]") {
  TempDir temp{"oran-bootstrap-automation-prompt-runner-session"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false, true, false);

    RecordingProvider recording{{text_response("first answer"), text_response("second answer")}};
    auto bridge = bootstrap::make_automation_agent_prompt_runner(base_bridge_options(io, assembly, cfg, recording));
    REQUIRE(bridge.has_value());

    auto first = co_await (*bridge)(automation::AutomationPromptRunRequest{
        .job_key = "cron:daily-summary",
        .job_type = automation::AutomationPromptJobType::cron,
        .agent_key = "automation",
        .prompt = "first prompt",
        .fired_at = at(std::chrono::minutes{1}),
    });
    REQUIRE(first.has_value());
    REQUIRE(first->text == "first answer");

    auto second = co_await (*bridge)(automation::AutomationPromptRunRequest{
        .job_key = "cron:daily-summary",
        .job_type = automation::AutomationPromptJobType::cron,
        .agent_key = "automation",
        .prompt = "second prompt",
        .fired_at = at(std::chrono::minutes{2}),
    });
    REQUIRE(second.has_value());
    REQUIRE(second->text == "second answer");

    const auto requests = recording.requests();
    REQUIRE(requests.size() == 2);
    REQUIRE(requests[0].messages.size() == 1);
    REQUIRE(requests[0].messages[0].blocks == core::Message::user_text("first prompt").blocks);
    REQUIRE(requests[1].messages.size() == 3);
    REQUIRE(requests[1].messages[0].blocks == core::Message::user_text("first prompt").blocks);
    REQUIRE(requests[1].messages[1].blocks == core::Message::assistant_text("first answer").blocks);
    REQUIRE(requests[1].messages[2].blocks == core::Message::user_text("second prompt").blocks);
  });
}

TEST_CASE("automation prompt bridge applies per-job agent overlays only when configured",
          "[unit][bootstrap][automation_prompt_runner][agent]") {
  TempDir temp{"oran-bootstrap-automation-prompt-runner-agent-overlay"};
  auto cfg = parse_config(R"json(
{
  "agents": {
    "writer": {
      "prompt_overlay": "Agent overlay: prefer concise, source-backed answers."
    }
  }
}
)json");

  test::run_async([&temp, &cfg](asio::io_context& io) -> async::Awaitable<void> {
    auto assembly = build_assembly(temp.path(), io, false);
    RecordingProvider recording{{text_response("writer done"), text_response("ghost done")}};
    auto bridge = bootstrap::make_automation_agent_prompt_runner(base_bridge_options(io, assembly, cfg, recording));
    REQUIRE(bridge.has_value());

    auto writer = co_await (*bridge)(automation::AutomationPromptRunRequest{
        .job_key = "cron:writer",
        .job_type = automation::AutomationPromptJobType::cron,
        .agent_key = "writer",
        .prompt = "draft release note",
        .fired_at = at(std::chrono::minutes{1}),
    });
    REQUIRE(writer.has_value());
    REQUIRE(writer->text == "writer done");

    auto ghost = co_await (*bridge)(automation::AutomationPromptRunRequest{
        .job_key = "cron:ghost",
        .job_type = automation::AutomationPromptJobType::cron,
        .agent_key = "ghost",
        .prompt = "draft other note",
        .fired_at = at(std::chrono::minutes{2}),
    });
    REQUIRE(ghost.has_value());
    REQUIRE(ghost->text == "ghost done");

    const auto requests = recording.requests();
    REQUIRE(requests.size() == 2);
    REQUIRE(requests[0].system_prompt.has_value());
    REQUIRE(requests[1].system_prompt.has_value());
    REQUIRE(requests[0].system_prompt->contains("Agent overlay: prefer concise, source-backed answers."));
    REQUIRE_FALSE(requests[1].system_prompt->contains("Agent overlay: prefer concise, source-backed answers."));
  });
}

TEST_CASE("automation prompt bridge keeps ask permissions fail-closed without an operator sink",
          "[unit][bootstrap][automation_prompt_runner][approval]") {
  TempDir temp{"oran-bootstrap-automation-prompt-runner-approval"};
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
    RecordingProvider recording{{
        provider::Response{
            .blocks = {core::ToolUseContent{
                .id = "read-1",
                .name = "file.read",
                .input_json = R"({"path":"note.txt"})",
            }},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::string{"fake-1"},
            .route_profile_used = std::nullopt,
        },
        text_response("recovered"),
    }};

    auto options = base_bridge_options(io, assembly, cfg, recording);
    options.mode = permission::Mode::strict;
    auto bridge = bootstrap::make_automation_agent_prompt_runner(std::move(options));
    REQUIRE(bridge.has_value());

    auto result = co_await (*bridge)(automation::AutomationPromptRunRequest{
        .job_key = "cron:read-note",
        .job_type = automation::AutomationPromptJobType::cron,
        .agent_key = "automation",
        .prompt = "read note",
        .fired_at = at(std::chrono::minutes{1}),
    });
    REQUIRE(result.has_value());
    REQUIRE(result->text == "recovered");

    const auto requests = recording.requests();
    REQUIRE(requests.size() == 2);
    const auto output = tool_result_output_in(requests[1], "read-1");
    REQUIRE(output.contains("tool error: tool requires approval"));
    REQUIRE(output.contains("reason: approval_required"));
  });
}

TEST_CASE("automation prompt bridge drives a cron service cycle through AutomationRuntime",
          "[unit][bootstrap][automation_prompt_runner][cron][runtime]") {
  TempDir temp{"oran-bootstrap-automation-prompt-runner-cron-runtime"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false, true, false);
    auto runtime = co_await automation::AutomationRuntime::open(
        io.get_executor(),
        automation::AutomationRuntimeOptions{.database_path = automation_db_path(temp)});
    REQUIRE(runtime.has_value());

    RecordingProvider recording{{text_response("cron done")}};
    auto bridge = bootstrap::make_automation_agent_prompt_runner(base_bridge_options(io, assembly, cfg, recording));
    REQUIRE(bridge.has_value());

    auto seed = automation::UpsertCronJobRequest{};
    seed.job_key = "cron:daily-summary";
    seed.agent_key = "automation";
    seed.agent_prompt = "Summarize daily activity.";
    seed.schedule.expression = "* * * * *";
    seed.schedule.first_fire_at = at(std::chrono::minutes{1});

    auto request = automation::CronServiceCycleRequest{};
    request.seeds.push_back(std::move(seed));
    request.now = at(std::chrono::minutes{2});
    request.max_iterations = 1;
    request.job_limit = 10;
    request.handler = automation::make_cron_prompt_handler(*bridge);

    auto result = co_await runtime->run_cron_service_cycle(std::move(request));

    REQUIRE(result.has_value());
    REQUIRE(result->seed_apply.requested_count == 1);
    REQUIRE(result->seed_apply.upserted_count == 1);
    REQUIRE(result->loop.attempted_count == 1);
    REQUIRE(result->loop.advanced_count == 1);

    const auto requests = recording.requests();
    REQUIRE(requests.size() == 1);
    REQUIRE(requests[0].messages.size() == 1);
    REQUIRE(requests[0].messages[0].blocks == core::Message::user_text("Summarize daily activity.").blocks);

    auto loaded = co_await runtime->repository().get_cron_job("cron:daily-summary");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    REQUIRE((*loaded)->state.last_fired_at == at(std::chrono::minutes{1}));
  });
}

TEST_CASE("automation prompt bridge drives triggered execution through TriggeredService",
          "[unit][bootstrap][automation_prompt_runner][triggered][runtime]") {
  TempDir temp{"oran-bootstrap-automation-prompt-runner-triggered-runtime"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false, true, false);
    auto runtime = co_await automation::AutomationRuntime::open(
        io.get_executor(),
        automation::AutomationRuntimeOptions{.database_path = automation_db_path(temp)});
    REQUIRE(runtime.has_value());
    REQUIRE((co_await runtime->repository().upsert_triggered_job(automation::UpsertTriggeredJobRequest{
                 .job_key = "triggered:webhook-ci",
                 .trigger_key = "webhook:ci",
                 .agent_key = "automation",
                 .agent_prompt = "Investigate the webhook payload.",
             }))
                .has_value());

    RecordingProvider recording{{text_response("triggered done")}};
    auto bridge = bootstrap::make_automation_agent_prompt_runner(base_bridge_options(io, assembly, cfg, recording));
    REQUIRE(bridge.has_value());

    auto request = automation::TriggeredExecuteRequest{};
    request.trigger_key = "webhook:ci";
    request.received_at = at(std::chrono::minutes{2});
    request.job_limit = 10;
    request.handler = automation::make_triggered_prompt_handler(*bridge);

    auto result = co_await runtime->triggered_service().execute(std::move(request));

    REQUIRE(result.has_value());
    REQUIRE(result->attempted_count == 1);
    REQUIRE(result->completed_count == 1);

    const auto requests = recording.requests();
    REQUIRE(requests.size() == 1);
    REQUIRE(requests[0].messages.size() == 1);
    REQUIRE(requests[0].messages[0].blocks == core::Message::user_text("Investigate the webhook payload.").blocks);

    auto runs = co_await runtime->repository().list_triggered_runs(automation::ListTriggeredRunsOptions{
        .job_key = "triggered:webhook-ci",
        .limit = 10,
    });
    REQUIRE(runs.has_value());
    REQUIRE(runs->size() == 1);
    REQUIRE(runs->front().outcome == automation::TriggeredRunOutcome::success);
  });
}
