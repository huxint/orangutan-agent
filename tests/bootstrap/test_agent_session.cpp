#include <algorithm>
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

#include <asio/any_io_executor.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <catch2/catch_test_macros.hpp>

#include <oran/agent.hpp>
#include <oran/async.hpp>
#include <oran/bootstrap.hpp>
#include <oran/config.hpp>
#include <oran/core/content.hpp>
#include <oran/core/error.hpp>
#include <oran/core/stop_reason.hpp>
#include <oran/hook.hpp>
#include <oran/memory.hpp>
#include <oran/permission.hpp>
#include <oran/provider.hpp>
#include <oran/storage.hpp>
#include <oran/tool.hpp>

#include "../test-helpers/run_async.hpp"

namespace agent = orangutan::agent;
namespace async = orangutan::async;
namespace bootstrap = orangutan::bootstrap;
namespace config = orangutan::config;
namespace core = orangutan::core;
namespace hook = orangutan::hook;
namespace memory = orangutan::memory;
namespace permission = orangutan::permission;
namespace provider = orangutan::provider;
namespace storage = orangutan::storage;
namespace test = orangutan::tests;
namespace tool = orangutan::tool;

namespace {

struct ProviderHookCapture {
  hook::Event event;
  hook::Payload payload;
};

struct MemoryHookCapture {
  hook::Event event;
  hook::Payload payload;
};

struct CapturingEventSink final : provider::EventSink {
  std::size_t text_deltas{0};
  std::size_t done_calls{0};
  std::string text;

  void on_text_delta(std::string_view delta) override {
    ++text_deltas;
    text += std::string{delta};
  }
  void on_done(core::StopReason /*stop_reason*/) override {
    ++done_calls;
  }
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

std::optional<std::string> tool_result_data_json_in(const provider::Request& request, std::string_view tool_use_id) {
  for (const auto& message : request.messages) {
    for (const auto& block : message.blocks) {
      const auto* result = std::get_if<core::ToolResultContent>(&block);
      if (result != nullptr && result->tool_use_id == tool_use_id) {
        return result->data_json;
      }
    }
  }
  return std::nullopt;
}

config::Config parse_config(std::string_view json) {
  auto parsed = config::Config::parse(json);
  REQUIRE(parsed.has_value());
  return std::move(*parsed);
}

bootstrap::RuntimeAssembly build_assembly(const std::filesystem::path& workspace,
                                          asio::any_io_executor executor,
                                          bool audit_enabled,
                                          bool session_memory_enabled = false,
                                          bool longterm_memory_enabled = true) {
  auto options = bootstrap::RuntimeAssemblyOptions{};
  options.audit_enabled = audit_enabled;
  options.session_memory_enabled = session_memory_enabled;
  options.longterm_memory_enabled = longterm_memory_enabled;
  auto assembly = bootstrap::RuntimeAssembly::build(workspace.string(), std::move(executor), std::move(options));
  REQUIRE(assembly.has_value());
  return std::move(*assembly);
}

bootstrap::RuntimeAssembly build_assembly(const std::filesystem::path& workspace,
                                          asio::io_context& io,
                                          bool audit_enabled,
                                          bool session_memory_enabled = false,
                                          bool longterm_memory_enabled = true) {
  return build_assembly(workspace, io.get_executor(), audit_enabled, session_memory_enabled, longterm_memory_enabled);
}

memory::longterm::Record make_longterm_record(std::string id, std::string body) {
  const auto created = core::Time{core::Time::time_point{std::chrono::seconds{1}}};
  const auto updated = core::Time{core::Time::time_point{std::chrono::seconds{2}}};
  return memory::longterm::Record{
      .key = memory::longterm::RecordKey{.id = std::move(id), .scope_key = "scope-A"},
      .kind = memory::longterm::RecordKind::project,
      .title = "Recall note",
      .body = std::move(body),
      .created_at = created,
      .updated_at = updated,
      .last_read_at = updated,
      .importance = 0.8,
      .tags = {"recall"},
      .linked_record_ids = {},
  };
}

bootstrap::AgentSessionOptions base_runner_options(asio::any_io_executor executor,
                                                   bootstrap::RuntimeAssembly& assembly,
                                                   config::Config& cfg,
                                                   provider::System& provider_system) {
  auto options = bootstrap::AgentSessionOptions{};
  options.blocking_executor = executor;
  options.executor = std::move(executor);
  options.assembly = &assembly;
  options.config = &cfg;
  options.provider = &provider_system;
  options.route = test_route();
  options.scope_key = "scope-A";
  options.agent_key = "coder";
  options.identity = "operator-1";
  options.origin = "test";
  return options;
}

bootstrap::AgentSessionOptions base_runner_options(asio::io_context& io,
                                                   bootstrap::RuntimeAssembly& assembly,
                                                   config::Config& cfg,
                                                   provider::System& provider_system) {
  return base_runner_options(io.get_executor(), assembly, cfg, provider_system);
}

hook::InProcessSink provider_capture_sink(std::vector<ProviderHookCapture>& captures) {
  return hook::InProcessSink{
      "provider-capture",
      [&captures](hook::Event event, hook::PayloadPtr payload) -> async::Awaitable<core::Result<void>> {
        captures.push_back(ProviderHookCapture{.event = event, .payload = *payload});
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

class MemoryCaptureSink final : public hook::Sink {
public:
  explicit MemoryCaptureSink(std::vector<MemoryHookCapture>& captures,
                             hook::HookDecision blocking_decision = hook::HookDecision{})
      : captures_{&captures}, blocking_decision_{std::move(blocking_decision)} {}

  [[nodiscard]] std::string_view id() const noexcept override {
    return "memory-capture";
  }

  [[nodiscard]] hook::SinkKind kind() const noexcept override {
    return hook::SinkKind::trusted_local;
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> receive(hook::Event event, hook::PayloadPtr payload) override {
    captures_->push_back(MemoryHookCapture{.event = event, .payload = *payload});
    co_return core::Result<void>{};
  }

  [[nodiscard]] async::Awaitable<core::Result<hook::HookDecision>> handle_blocking(hook::Event event,
                                                                                   hook::PayloadPtr payload) override {
    captures_->push_back(MemoryHookCapture{.event = event, .payload = *payload});
    co_return blocking_decision_;
  }

private:
  std::vector<MemoryHookCapture>* captures_;
  hook::HookDecision blocking_decision_;
};

}  // namespace

TEST_CASE("AgentSession rejects unknown permission overlays", "[unit][bootstrap][prompt_runner]") {
  TempDir temp{"oran-bootstrap-prompt-runner-bad-agent"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    provider::FakeProvider fake{std::vector<provider::ScriptedTurn>{}};
    auto options = base_runner_options(io, assembly, cfg, fake);
    options.agent_config_name = "ghost";

    auto runner = bootstrap::AgentSession::create(std::move(options));

    REQUIRE_FALSE(runner.has_value());
    REQUIRE(runner.error().kind() == core::ErrorKind::not_found);
    co_return;
  });
}

TEST_CASE("AgentSession rejects an empty executor at create time", "[unit][bootstrap][prompt_runner]") {
  TempDir temp{"oran-bootstrap-prompt-runner-empty-executor"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    provider::FakeProvider fake{std::vector<provider::ScriptedTurn>{}};
    auto options = base_runner_options(io, assembly, cfg, fake);
    options.executor = asio::any_io_executor{};

    auto runner = bootstrap::AgentSession::create(std::move(options));

    REQUIRE_FALSE(runner.has_value());
    REQUIRE(runner.error().kind() == core::ErrorKind::invalid_argument);
    co_return;
  });
}

TEST_CASE("AgentSession drives prompts through the agent loop and trace writer", "[unit][bootstrap][prompt_runner]") {
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

    auto runner = bootstrap::AgentSession::create(base_runner_options(io, assembly, cfg, fake));
    REQUIRE(runner.has_value());

    auto request = agent::PromptRequest{.prompt = "hello"};
    auto result = co_await (*runner)->run_prompt(std::move(request));

    REQUIRE(result.has_value());
    REQUIRE(fake.turns_consumed() == 1);
    REQUIRE(assembly.trace_repository() != nullptr);
    auto count = co_await assembly.trace_repository()->count_turns();
    REQUIRE(count.has_value());
    REQUIRE(*count == 1);
  });
}

TEST_CASE("AgentSession streams deltas to an injected event sink", "[unit][bootstrap][prompt_runner]") {
  TempDir temp{"oran-bootstrap-prompt-runner-injected-sink"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response = std::nullopt,
        .deltas = {provider::TextDelta{.text = "Hel"},
                   provider::TextDelta{.text = "lo"},
                   provider::StreamEnd{.stop_reason = core::StopReason::end_turn,
                                       .usage = std::nullopt,
                                       .model_used = std::nullopt}},
        .error = std::nullopt,
        .latency = {},
    });
    provider::FakeProvider fake{std::move(plan)};

    CapturingEventSink sink;
    auto options = base_runner_options(io, assembly, cfg, fake);
    options.event_sink = &sink;
    auto runner = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(runner.has_value());

    auto request = agent::PromptRequest{.prompt = "hello"};
    auto result = co_await (*runner)->run_prompt(std::move(request));

    REQUIRE(result.has_value());
    REQUIRE(sink.text_deltas == 2);
    REQUIRE(sink.text == "Hello");
    REQUIRE(sink.done_calls == 1);
    REQUIRE(fake.turns_consumed() == 1);
  });
}

TEST_CASE("AgentSession persists successful turns through the session store",
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

    auto runner = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(runner.has_value());
    auto result = co_await (*runner)->run_prompt(orangutan::agent::PromptRequest{.prompt = "remember"});

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

TEST_CASE("AgentSession reloads persisted history for a new runner instance",
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
      auto runner = bootstrap::AgentSession::create(std::move(options));
      REQUIRE(runner.has_value());
      auto first = co_await (*runner)->run_prompt(orangutan::agent::PromptRequest{.prompt = "first prompt"});
      REQUIRE(first.has_value());
    }

    RecordingProvider recording{{text_response("second answer")}};
    auto options = base_runner_options(io, assembly, cfg, recording);
    options.session_id = session_id;
    auto runner = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(runner.has_value());

    auto second = co_await (*runner)->run_prompt(orangutan::agent::PromptRequest{.prompt = "second prompt"});

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

TEST_CASE("AgentSession uses provider execution retry before returning text",
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

    auto runner = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(runner.has_value());
    auto prompt = orangutan::agent::PromptRequest{.prompt = "retry"};
    auto result = co_await (*runner)->run_prompt(std::move(prompt));

    REQUIRE(result.has_value());
    REQUIRE(result->text == "retried");
    REQUIRE(fake.turns_consumed() == 2);
  });
}

TEST_CASE("AgentSession publishes provider hooks through RuntimeAssembly",
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

    auto runner = bootstrap::AgentSession::create(base_runner_options(io, assembly, cfg, fake));
    REQUIRE(runner.has_value());
    auto prompt = orangutan::agent::PromptRequest{.prompt = "hooks"};
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
    REQUIRE(request->origin == "test");
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

TEST_CASE("AgentSession feeds ToolSearch results back into per-session state",
          "[unit][bootstrap][prompt_runner][session_state]") {
  TempDir temp{"oran-bootstrap-prompt-runner-observe"};
  test::run_async(
      [&temp](asio::io_context& io) -> async::Awaitable<void> {
        auto cfg = config::Config{};
        auto assembly = build_assembly(temp.path(), io, false);
        std::vector<provider::ScriptedTurn> plan;
        plan.push_back(provider::ScriptedTurn{
            .response =
                provider::Response{
                    .blocks = {core::ToolUseContent{
                        .id = "search-1",
                        .name = "ToolSearch",
                        .input_json = R"({"name":"FileRead"})",
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

        auto runner = bootstrap::AgentSession::create(base_runner_options(io, assembly, cfg, fake));
        REQUIRE(runner.has_value());
        auto prompt = orangutan::agent::PromptRequest{.prompt = "search"};
        auto result = co_await (*runner)->run_prompt(std::move(prompt));

        REQUIRE(result.has_value());
        REQUIRE(result->text == "searched");
        REQUIRE(fake.turns_consumed() == 2);
      },
      std::chrono::seconds{3});
}

TEST_CASE("AgentSession renders memory framing once per prompt before loop iterations",
          "[unit][bootstrap][prompt_runner][memory]") {
  TempDir temp{"oran-bootstrap-prompt-runner-memory-framing"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    write_file(temp.path() / "note.txt", "memory framing fixture\n");

    RecordingProvider recording{{
        provider::Response{
            .blocks = {core::ToolUseContent{
                .id = "read-1",
                .name = "FileRead",
                .input_json = R"({"path":"note.txt"})",
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
        text_response("done"),
    }};

    auto options = base_runner_options(io, assembly, cfg, recording);
    options.memory_framing = "memory: stable";
    auto runner = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(runner.has_value());

    auto result = co_await (*runner)->run_prompt(orangutan::agent::PromptRequest{.prompt = "read"});

    REQUIRE(result.has_value());
    REQUIRE(result->text == "done");

    const auto requests = recording.requests();
    REQUIRE(requests.size() == 2);
    REQUIRE(requests[0].system_prompt.has_value());
    REQUIRE(requests[1].system_prompt.has_value());
    REQUIRE(requests[0].system_prompt->contains("memory: stable"));
    REQUIRE(requests[1].system_prompt->contains("memory: stable"));
    REQUIRE(*requests[0].system_prompt == *requests[1].system_prompt);
  });
}

TEST_CASE("AgentSession leaves long-term recall disabled by default", "[unit][bootstrap][prompt_runner][memory]") {
  TempDir temp{"oran-bootstrap-prompt-runner-longterm-default-off"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    REQUIRE(assembly.longterm_memory_backend() != nullptr);
    auto upserted = co_await assembly.longterm_memory_backend()->upsert(memory::longterm::WriteRequest{
        .record = make_longterm_record("lt-default-off", "Recall should stay out unless explicitly enabled."),
    });
    REQUIRE(upserted.has_value());

    RecordingProvider recording{{text_response("done")}};
    auto runner = bootstrap::AgentSession::create(base_runner_options(io, assembly, cfg, recording));
    REQUIRE(runner.has_value());

    auto result = co_await (*runner)->run_prompt(orangutan::agent::PromptRequest{.prompt = "recall"});

    REQUIRE(result.has_value());
    const auto requests = recording.requests();
    REQUIRE(requests.size() == 1);
    REQUIRE(requests[0].system_prompt.has_value());
    REQUIRE_FALSE(requests[0].system_prompt->contains("Long-term memory:"));
  });
}

TEST_CASE("AgentSession recalls long-term memory once before loop iterations",
          "[integration][bootstrap][prompt_runner][memory][core_boundary]") {
  TempDir temp{"oran-bootstrap-prompt-runner-longterm-recall"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    REQUIRE(assembly.longterm_memory_backend() != nullptr);
    auto upserted = co_await assembly.longterm_memory_backend()->upsert(memory::longterm::WriteRequest{
        .record = make_longterm_record("lt-recall-1", "Recall plumbing reaches the prompt boundary."),
    });
    REQUIRE(upserted.has_value());
    auto foreign = make_longterm_record("foreign", "Recall plumbing belongs to another workspace.");
    foreign.key.scope_key = "scope-B";
    auto foreign_saved = co_await assembly.longterm_memory_backend()->upsert({.record = std::move(foreign)});
    REQUIRE(foreign_saved.has_value());
    std::vector<MemoryHookCapture> hook_captures;
    MemoryCaptureSink sink{hook_captures};
    assembly.hook_bus().bind(sink, {hook::Event::memory_read_after});
    write_file(temp.path() / "note.txt", "long-term recall fixture\n");

    RecordingProvider recording{{
        provider::Response{
            .blocks = {core::ToolUseContent{
                .id = "read-1",
                .name = "FileRead",
                .input_json = R"({"path":"note.txt"})",
            }},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::string{"fake-1"},
            .route_profile_used = std::nullopt,
        },
        text_response("done"),
    }};

    auto options = base_runner_options(io, assembly, cfg, recording);
    options.longterm_recall = bootstrap::LongtermRecallOptions{.enabled = true, .limit = 5};
    auto runner = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(runner.has_value());

    auto result = co_await (*runner)->run_prompt(orangutan::agent::PromptRequest{.prompt = "recall plumbing"});

    REQUIRE(result.has_value());
    REQUIRE(result->text == "done");
    const auto requests = recording.requests();
    REQUIRE(requests.size() == 2);
    REQUIRE(requests[0].system_prompt.has_value());
    REQUIRE(requests[1].system_prompt.has_value());
    REQUIRE(requests[0].system_prompt->contains("Long-term memory:"));
    REQUIRE(requests[0].system_prompt->contains("Recall plumbing reaches the prompt boundary."));
    REQUIRE_FALSE(requests[0].system_prompt->contains("another workspace"));
    REQUIRE(requests[0].system_prompt->contains("tags: recall"));
    REQUIRE(*requests[0].system_prompt == *requests[1].system_prompt);

    REQUIRE(hook_captures.size() == 1);
    REQUIRE(hook_captures[0].event == hook::Event::memory_read_after);
    const auto* read = std::get_if<hook::MemoryReadPayload>(&hook_captures[0].payload);
    REQUIRE(read != nullptr);
    REQUIRE(read->who.scope_key == "scope-A");
    REQUIRE(read->who.agent_key == "coder");
    REQUIRE(read->who.identity == "operator-1");
    REQUIRE(read->source == "MemoryRecall");
    REQUIRE(read->query == "recall plumbing");
    REQUIRE(read->redacted_query_bytes == std::string_view{"recall plumbing"}.size());
    REQUIRE(read->limit == 5);
    REQUIRE(read->kinds.empty());
    REQUIRE(read->match_count == 1);
    REQUIRE(read->hits.size() == 1);
    REQUIRE(read->hits[0].record.id == "lt-recall-1");
    REQUIRE(read->hits[0].record.body == "Recall plumbing reaches the prompt boundary.");
    REQUIRE(read->hits[0].record.tags == std::vector<std::string>{"recall"});
    REQUIRE(read->hits[0].redacted_record.has_value());
    REQUIRE(read->hits[0].redacted_record->body_bytes ==
            std::string_view{"Recall plumbing reaches the prompt boundary."}.size());
    REQUIRE(read->finished_at >= read->started_at);
  });
}

TEST_CASE("AgentSession dispatches MemoryRecall through long-term runtime",
          "[unit][bootstrap][prompt_runner][memory]") {
  TempDir temp{"oran-bootstrap-prompt-runner-memory-recall-tool"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    REQUIRE(assembly.longterm_memory_backend() != nullptr);
    auto upserted = co_await assembly.longterm_memory_backend()->upsert(memory::longterm::WriteRequest{
        .record = make_longterm_record("lt-tool-recall", "Memory tool found toolrecallanchor in the project."),
    });
    REQUIRE(upserted.has_value());
    std::vector<MemoryHookCapture> hook_captures;
    MemoryCaptureSink sink{hook_captures};
    assembly.hook_bus().bind(sink, {hook::Event::memory_read_after});

    RecordingProvider recording{{
        provider::Response{
            .blocks = {core::ToolUseContent{
                .id = "memory-1",
                .name = "MemoryRecall",
                .input_json = R"({"query":"toolrecallanchor","limit":5,"kinds":["project"]})",
            }},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::string{"fake-1"},
            .route_profile_used = std::nullopt,
        },
        text_response("done"),
    }};

    auto runner = bootstrap::AgentSession::create(base_runner_options(io, assembly, cfg, recording));
    REQUIRE(runner.has_value());

    auto result = co_await (*runner)->run_prompt(orangutan::agent::PromptRequest{.prompt = "use memory"});

    REQUIRE(result.has_value());
    REQUIRE(result->text == "done");
    const auto requests = recording.requests();
    REQUIRE(requests.size() == 2);
    const auto output = tool_result_output_in(requests[1], "memory-1");
    REQUIRE(output.contains("MemoryRecall: 1 match"));
    REQUIRE(output.contains("Memory tool found toolrecallanchor in the project."));
    const auto data_json = tool_result_data_json_in(requests[1], "memory-1");
    REQUIRE(data_json.has_value());
    REQUIRE(data_json->contains(R"("kind":"memory_recall")"));
    REQUIRE(data_json->contains(R"("id":"lt-tool-recall")"));
    REQUIRE(data_json->contains(R"("match_count":1)"));

    REQUIRE(hook_captures.size() == 1);
    REQUIRE(hook_captures[0].event == hook::Event::memory_read_after);
    const auto* read = std::get_if<hook::MemoryReadPayload>(&hook_captures[0].payload);
    REQUIRE(read != nullptr);
    REQUIRE(read->who.scope_key == "scope-A");
    REQUIRE(read->who.agent_key == "coder");
    REQUIRE(read->who.identity == "operator-1");
    REQUIRE(read->source == "MemoryRecall");
    REQUIRE(read->query == "toolrecallanchor");
    REQUIRE(read->redacted_query_bytes == std::string_view{"toolrecallanchor"}.size());
    REQUIRE(read->limit == 5);
    REQUIRE(read->kinds == std::vector<std::string>{"project"});
    REQUIRE(read->match_count == 1);
    REQUIRE(read->hits.size() == 1);
    REQUIRE(read->hits[0].record.id == "lt-tool-recall");
    REQUIRE(read->hits[0].record.body == "Memory tool found toolrecallanchor in the project.");
    REQUIRE(read->hits[0].record.tags == std::vector<std::string>{"recall"});
    REQUIRE(read->hits[0].redacted_record.has_value());
    REQUIRE(read->hits[0].redacted_record->body_bytes ==
            std::string_view{"Memory tool found toolrecallanchor in the project."}.size());
    REQUIRE(read->finished_at >= read->started_at);
  });
}

TEST_CASE("AgentSession dispatches MemoryRemember through long-term backend",
          "[unit][bootstrap][prompt_runner][memory]") {
  TempDir temp{"oran-bootstrap-prompt-runner-memory-remember-tool"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    REQUIRE(assembly.longterm_memory_backend() != nullptr);
    std::vector<MemoryHookCapture> hook_captures;
    MemoryCaptureSink sink{hook_captures};
    assembly.hook_bus().bind(sink, {hook::Event::memory_write_before, hook::Event::memory_write_after});

    RecordingProvider recording{{
        provider::Response{
            .blocks = {core::ToolUseContent{
                .id = "memory-write-1",
                .name = "MemoryRemember",
                .input_json =
                    R"({"id":"lt-tool-remember","kind":"project","title":"Remembered note","body":"Memory remember wrote rememberanchor into the project.","importance":0.75,"tags":["remember","tool"],"linked_record_ids":["lt-tool-recall"]})",
            }},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::string{"fake-1"},
            .route_profile_used = std::nullopt,
        },
        text_response("done"),
    }};

    auto options = base_runner_options(io, assembly, cfg, recording);
    options.mode = permission::Mode::permissive;
    auto runner = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(runner.has_value());

    auto result = co_await (*runner)->run_prompt(orangutan::agent::PromptRequest{.prompt = "write memory"});

    REQUIRE(result.has_value());
    REQUIRE(result->text == "done");
    const auto requests = recording.requests();
    REQUIRE(requests.size() == 2);
    const auto output = tool_result_output_in(requests[1], "memory-write-1");
    REQUIRE(output.contains("MemoryRemember: saved project record lt-tool-remember"));
    REQUIRE(output.contains("title: Remembered note"));
    const auto data_json = tool_result_data_json_in(requests[1], "memory-write-1");
    REQUIRE(data_json.has_value());
    REQUIRE(data_json->contains(R"("kind":"memory_remember")"));
    REQUIRE(data_json->contains(R"("id":"lt-tool-remember")"));
    REQUIRE(data_json->contains(R"("scope_key":"scope-A")"));
    REQUIRE(data_json->contains(R"("tags":["remember","tool"])"));

    auto stored = co_await assembly.longterm_memory_backend()->get(memory::longterm::RecordKey{
        .id = "lt-tool-remember",
        .scope_key = "scope-A",
    });
    REQUIRE(stored.has_value());
    REQUIRE(stored->kind == memory::longterm::RecordKind::project);
    REQUIRE(stored->title == "Remembered note");
    REQUIRE(stored->body == "Memory remember wrote rememberanchor into the project.");
    REQUIRE(stored->importance == 0.75);
    REQUIRE(stored->tags == std::vector<std::string>{"remember", "tool"});
    REQUIRE(stored->linked_record_ids == std::vector<std::string>{"lt-tool-recall"});

    REQUIRE(hook_captures.size() == 2);
    REQUIRE(hook_captures[0].event == hook::Event::memory_write_before);
    const auto* before = std::get_if<hook::MemoryWritePayload>(&hook_captures[0].payload);
    REQUIRE(before != nullptr);
    REQUIRE(before->who.scope_key == "scope-A");
    REQUIRE(before->who.agent_key == "coder");
    REQUIRE(before->who.identity == "operator-1");
    REQUIRE(before->record.id == "lt-tool-remember");
    REQUIRE(before->record.scope_key == "scope-A");
    REQUIRE(before->record.kind == "project");
    REQUIRE(before->record.title == "Remembered note");
    REQUIRE(before->record.body == "Memory remember wrote rememberanchor into the project.");
    REQUIRE(before->record.tags == std::vector<std::string>{"remember", "tool"});
    REQUIRE(before->redacted_record.has_value());
    REQUIRE(before->redacted_record->body_bytes ==
            std::string_view{"Memory remember wrote rememberanchor into the project."}.size());

    REQUIRE(hook_captures[1].event == hook::Event::memory_write_after);
    const auto* after = std::get_if<hook::MemoryWritePayload>(&hook_captures[1].payload);
    REQUIRE(after != nullptr);
    REQUIRE(after->record.id == "lt-tool-remember");
    REQUIRE(after->record.body == "Memory remember wrote rememberanchor into the project.");
    REQUIRE(after->finished_at >= after->started_at);
  });
}

TEST_CASE("AgentSession lets MemoryWrite.before veto MemoryRemember",
          "[unit][bootstrap][prompt_runner][memory][hook]") {
  TempDir temp{"oran-bootstrap-prompt-runner-memory-write-veto"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    REQUIRE(assembly.longterm_memory_backend() != nullptr);
    std::vector<MemoryHookCapture> hook_captures;
    auto veto = hook::HookDecision{};
    veto.kind = hook::HookDecisionKind::veto;
    veto.reason = "memory policy";
    MemoryCaptureSink sink{hook_captures, std::move(veto)};
    assembly.hook_bus().bind(sink, {hook::Event::memory_write_before, hook::Event::memory_write_after});

    RecordingProvider recording{{
        provider::Response{
            .blocks = {core::ToolUseContent{
                .id = "memory-write-veto-1",
                .name = "MemoryRemember",
                .input_json =
                    R"({"id":"lt-tool-remember-veto","kind":"project","title":"Blocked note","body":"This should not persist."})",
            }},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::string{"fake-1"},
            .route_profile_used = std::nullopt,
        },
        text_response("done"),
    }};

    auto options = base_runner_options(io, assembly, cfg, recording);
    options.mode = permission::Mode::permissive;
    auto runner = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(runner.has_value());

    auto result = co_await (*runner)->run_prompt(orangutan::agent::PromptRequest{.prompt = "write blocked memory"});

    REQUIRE(result.has_value());
    REQUIRE(result->text == "done");
    const auto requests = recording.requests();
    REQUIRE(requests.size() == 2);
    const auto output = tool_result_output_in(requests[1], "memory-write-veto-1");
    REQUIRE(output.contains("memory write blocked by hook"));
    REQUIRE(output.contains("memory policy"));

    auto stored = co_await assembly.longterm_memory_backend()->get(memory::longterm::RecordKey{
        .id = "lt-tool-remember-veto",
        .scope_key = "scope-A",
    });
    REQUIRE_FALSE(stored.has_value());
    REQUIRE(stored.error().kind() == core::ErrorKind::not_found);

    REQUIRE(hook_captures.size() == 1);
    REQUIRE(hook_captures[0].event == hook::Event::memory_write_before);
    const auto* before = std::get_if<hook::MemoryWritePayload>(&hook_captures[0].payload);
    REQUIRE(before != nullptr);
    REQUIRE(before->record.id == "lt-tool-remember-veto");
    REQUIRE(before->record.body == "This should not persist.");
  });
}

TEST_CASE("AgentSession dispatches MemoryForget through long-term backend",
          "[unit][bootstrap][prompt_runner][memory]") {
  TempDir temp{"oran-bootstrap-prompt-runner-memory-forget-tool"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    REQUIRE(assembly.longterm_memory_backend() != nullptr);
    std::vector<MemoryHookCapture> hook_captures;
    MemoryCaptureSink sink{hook_captures};
    assembly.hook_bus().bind(sink, {hook::Event::memory_forget});
    auto upserted = co_await assembly.longterm_memory_backend()->upsert(memory::longterm::WriteRequest{
        .record = make_longterm_record("lt-tool-forget", "Memory forget should remove forgetanchor."),
    });
    REQUIRE(upserted.has_value());

    RecordingProvider recording{{
        provider::Response{
            .blocks = {core::ToolUseContent{
                .id = "memory-forget-1",
                .name = "MemoryForget",
                .input_json = R"({"id":"lt-tool-forget"})",
            }},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::string{"fake-1"},
            .route_profile_used = std::nullopt,
        },
        text_response("done"),
    }};

    auto options = base_runner_options(io, assembly, cfg, recording);
    options.mode = permission::Mode::permissive;
    auto runner = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(runner.has_value());

    auto result = co_await (*runner)->run_prompt(orangutan::agent::PromptRequest{.prompt = "forget memory"});

    REQUIRE(result.has_value());
    REQUIRE(result->text == "done");
    const auto requests = recording.requests();
    REQUIRE(requests.size() == 2);
    const auto output = tool_result_output_in(requests[1], "memory-forget-1");
    REQUIRE(output.contains("MemoryForget: removed record lt-tool-forget"));
    const auto data_json = tool_result_data_json_in(requests[1], "memory-forget-1");
    REQUIRE(data_json.has_value());
    REQUIRE(data_json->contains(R"("kind":"memory_forget")"));
    REQUIRE(data_json->contains(R"("id":"lt-tool-forget")"));
    REQUIRE(data_json->contains(R"("scope_key":"scope-A")"));

    auto stored = co_await assembly.longterm_memory_backend()->get(memory::longterm::RecordKey{
        .id = "lt-tool-forget",
        .scope_key = "scope-A",
    });
    REQUIRE_FALSE(stored.has_value());
    REQUIRE(stored.error().kind() == core::ErrorKind::not_found);

    REQUIRE(hook_captures.size() == 1);
    REQUIRE(hook_captures[0].event == hook::Event::memory_forget);
    const auto* forget = std::get_if<hook::MemoryForgetPayload>(&hook_captures[0].payload);
    REQUIRE(forget != nullptr);
    REQUIRE(forget->who.scope_key == "scope-A");
    REQUIRE(forget->who.agent_key == "coder");
    REQUIRE(forget->who.identity == "operator-1");
    REQUIRE(forget->id == "lt-tool-forget");
    REQUIRE(forget->scope_key == "scope-A");
    REQUIRE(forget->finished_at >= forget->started_at);
  });
}

TEST_CASE("AgentSession rejects long-term recall without assembly runtime",
          "[unit][bootstrap][prompt_runner][memory]") {
  TempDir temp{"oran-bootstrap-prompt-runner-longterm-no-runtime"};
  asio::io_context io;
  auto cfg = config::Config{};
  auto assembly = build_assembly(temp.path(), io, false, false, false);
  RecordingProvider recording{{text_response("unused")}};

  auto options = base_runner_options(io, assembly, cfg, recording);
  options.longterm_recall = bootstrap::LongtermRecallOptions{.enabled = true, .limit = 5};
  auto runner = bootstrap::AgentSession::create(std::move(options));

  REQUIRE_FALSE(runner.has_value());
  REQUIRE(runner.error().kind() == core::ErrorKind::invalid_argument);
}

TEST_CASE("AgentSession renders default system preamble once per prompt before loop iterations",
          "[unit][bootstrap][prompt_runner][prompt]") {
  TempDir temp{"oran-bootstrap-prompt-runner-system-preamble"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    write_file(temp.path() / "note.txt", "system preamble fixture\n");

    RecordingProvider recording{{
        provider::Response{
            .blocks = {core::ToolUseContent{
                .id = "read-1",
                .name = "FileRead",
                .input_json = R"({"path":"note.txt"})",
            }},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::string{"fake-1"},
            .route_profile_used = std::nullopt,
        },
        text_response("done"),
    }};

    auto options = base_runner_options(io, assembly, cfg, recording);
    auto runner = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(runner.has_value());

    auto result = co_await (*runner)->run_prompt(orangutan::agent::PromptRequest{.prompt = "read"});

    REQUIRE(result.has_value());
    REQUIRE(result->text == "done");

    const auto requests = recording.requests();
    REQUIRE(requests.size() == 2);
    REQUIRE(requests[0].system_prompt.has_value());
    REQUIRE(requests[1].system_prompt.has_value());
    REQUIRE(requests[0].system_prompt->contains("You are Orangutan"));
    REQUIRE(requests[0].system_prompt->contains("Operating principles:"));
    REQUIRE(requests[0].system_prompt->contains("Tool: FileRead"));
    REQUIRE(*requests[0].system_prompt == *requests[1].system_prompt);
  });
}

TEST_CASE("AgentSession renders selected agent prompt overlay in the stable prefix",
          "[unit][bootstrap][prompt_runner][prompt]") {
  TempDir temp{"oran-bootstrap-prompt-runner-agent-overlay"};
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
    write_file(temp.path() / "note.txt", "agent overlay fixture\n");

    RecordingProvider recording{{
        provider::Response{
            .blocks = {core::ToolUseContent{
                .id = "read-1",
                .name = "FileRead",
                .input_json = R"({"path":"note.txt"})",
            }},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::string{"fake-1"},
            .route_profile_used = std::nullopt,
        },
        text_response("done"),
    }};

    auto options = base_runner_options(io, assembly, cfg, recording);
    options.agent_config_name = "writer";
    auto runner = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(runner.has_value());

    auto result = co_await (*runner)->run_prompt(orangutan::agent::PromptRequest{.prompt = "read"});

    REQUIRE(result.has_value());
    REQUIRE(result->text == "done");

    const auto requests = recording.requests();
    REQUIRE(requests.size() == 2);
    REQUIRE(requests[0].system_prompt.has_value());
    REQUIRE(requests[1].system_prompt.has_value());
    REQUIRE(requests[0].system_prompt->contains("Agent overlay: prefer concise, source-backed answers."));
    REQUIRE(*requests[0].system_prompt == *requests[1].system_prompt);
  });
}

TEST_CASE("AgentSession dispatches through an injected shared scheduler",
          "[unit][bootstrap][prompt_runner][scheduler]") {
  TempDir temp{"oran-bootstrap-prompt-runner-injected-scheduler"};
  write_file(temp.path() / "note.txt", "injected scheduler fixture\n");
  test::run_async(
      [&temp](asio::io_context& io) -> async::Awaitable<void> {
        auto cfg = config::Config{};
        auto assembly = build_assembly(temp.path(), io, false);

        // One registry + scheduler owned by the test and injected into the
        // runner — exactly how `--serve` shares them across per-job runners.
        tool::Registry registry;
        REQUIRE(tool::register_builtins(registry).has_value());
        agent::ToolScheduler scheduler{io.get_executor(), registry};

        RecordingProvider recording{{
            provider::Response{
                .blocks = {core::ToolUseContent{
                    .id = "read-1",
                    .name = "FileRead",
                    .input_json = R"({"path":"note.txt"})",
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
            text_response("read"),
        }};

        auto options = base_runner_options(io, assembly, cfg, recording);
        options.registry = &registry;
        options.scheduler = &scheduler;
        auto runner = bootstrap::AgentSession::create(std::move(options));
        REQUIRE(runner.has_value());

        auto result = co_await (*runner)->run_prompt(orangutan::agent::PromptRequest{.prompt = "read"});
        REQUIRE(result.has_value());
        REQUIRE(result->text == "read");

        // The FileRead ran through the *injected* scheduler, so its shared
        // (read) lock acquire is visible on the scheduler the test owns —
        // proof the runner borrowed it instead of building its own.
        REQUIRE(scheduler.lock_stats().shared_acquires >= 1);
      },
      std::chrono::seconds{3});
}

TEST_CASE("AgentSession::create rejects a registry/scheduler supplied without its pair",
          "[unit][bootstrap][prompt_runner][scheduler]") {
  TempDir temp{"oran-bootstrap-prompt-runner-injection-validate"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    provider::FakeProvider fake{std::vector<provider::ScriptedTurn>{}};

    tool::Registry registry;
    REQUIRE(tool::register_builtins(registry).has_value());
    agent::ToolScheduler scheduler{io.get_executor(), registry};

    // A scheduler without its registry (and vice versa) is rejected up front:
    // the runner's `.tools` and the scheduler's registry must be the same
    // instance, so the pair is all-or-nothing.
    auto only_scheduler = base_runner_options(io, assembly, cfg, fake);
    only_scheduler.scheduler = &scheduler;
    REQUIRE_FALSE(bootstrap::AgentSession::create(std::move(only_scheduler)).has_value());

    auto only_registry = base_runner_options(io, assembly, cfg, fake);
    only_registry.registry = &registry;
    REQUIRE_FALSE(bootstrap::AgentSession::create(std::move(only_registry)).has_value());

    co_return;
  });
}

// The session and scheduler share a strand while multiple IO workers drive
// the runtime. Concurrent tool calls must preserve that coordinating boundary.
TEST_CASE("AgentSession multi-tool batches complete on a multi-worker runtime",
          "[unit][bootstrap][prompt_runner][scheduler][concurrency]") {
  TempDir temp{"oran-bootstrap-prompt-runner-multiworker"};
  write_file(temp.path() / "left.txt", "left fixture\n");
  write_file(temp.path() / "right.txt", "right fixture\n");

  auto runtime = async::Runtime{async::RuntimeConfig{.io_workers = 4, .cpu_workers = 1}};
  auto agent_strand = runtime.make_strand();

  auto cfg = config::Config{};
  auto assembly = build_assembly(temp.path(), runtime.executor(), false);

  // Each scripted turn fans two parallel FileRead calls through the
  // scheduler before the terminal text turn ends the loop (16-iteration cap).
  constexpr int kToolTurns = 8;
  std::vector<provider::ScriptedTurn> plan;
  for (int turn = 0; turn < kToolTurns; ++turn) {
    plan.push_back(provider::ScriptedTurn{
        .response =
            provider::Response{
                .blocks = {core::ToolUseContent{.id = "read-left-" + std::to_string(turn),
                                                .name = "FileRead",
                                                .input_json = R"({"path":"left.txt"})"},
                           core::ToolUseContent{.id = "read-right-" + std::to_string(turn),
                                                .name = "FileRead",
                                                .input_json = R"({"path":"right.txt"})"}},
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
        .latency = std::chrono::milliseconds{1},
    });
  }
  plan.push_back(provider::ScriptedTurn{
      .response = text_response("multiworker done"),
      .deltas = {},
      .error = std::nullopt,
      .latency = {},
  });
  provider::FakeProvider fake{std::move(plan)};

  auto runner = bootstrap::AgentSession::create(base_runner_options(agent_strand, assembly, cfg, fake));
  REQUIRE(runner.has_value());

  // Watchdog so a lost completion fails loudly instead of hanging the bucket.
  bool timed_out = false;
  asio::steady_timer watchdog{runtime.executor()};
  watchdog.expires_after(std::chrono::seconds{20});
  watchdog.async_wait([&](const asio::error_code& ec) {
    if (!ec) {
      timed_out = true;
      runtime.stop();
    }
  });

  auto result = std::optional<core::Result<orangutan::agent::PromptResult>>{};
  asio::co_spawn(
      agent_strand,
      [&]() -> async::Awaitable<void> {
        result = co_await (*runner)->run_prompt(orangutan::agent::PromptRequest{.prompt = "read both"});
        watchdog.cancel();
        runtime.stop();
        co_return;
      },
      asio::detached);

  REQUIRE(runtime.run().has_value());
  REQUIRE_FALSE(timed_out);
  REQUIRE(result.has_value());
  REQUIRE(result->has_value());
  REQUIRE((*result)->text == "multiworker done");
  REQUIRE(fake.turns_consumed() == kToolTurns + 1);
}

TEST_CASE("automatic recall refuses denied memory before calling the provider",
          "[integration][bootstrap][memory][core_boundary]") {
  TempDir temp{"oran-recall-policy"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = parse_config(R"({"permissions":{"deny":[{"tool_pattern":"MemoryRecall"}]}})");
    auto assembly = build_assembly(temp.path(), io, false);
    auto saved = co_await assembly.longterm_memory_backend()->upsert({
        .record = make_longterm_record("private", "Private recall content"),
    });
    REQUIRE(saved.has_value());
    std::vector<MemoryHookCapture> captures;
    MemoryCaptureSink sink{captures};
    assembly.hook_bus().bind(sink, {hook::Event::memory_read_after});
    RecordingProvider recording{{text_response("unexpected")}};
    auto options = base_runner_options(io, assembly, cfg, recording);
    options.longterm_recall.enabled = true;
    auto session = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(session.has_value());

    auto result = co_await (*session)->run_prompt({.prompt = "Private recall"});

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind() == core::ErrorKind::permission_denied);
    CHECK(recording.requests().empty());
    CHECK(captures.empty());
  });
}

TEST_CASE("a session advertises memory tools only when memory is available",
          "[integration][bootstrap][memory][core_boundary]") {
  TempDir temp{"oran-tool-services"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false, false, false);
    RecordingProvider recording{{text_response("done")}};
    auto session = bootstrap::AgentSession::create(base_runner_options(io, assembly, cfg, recording));
    REQUIRE(session.has_value());

    auto result = co_await (*session)->run_prompt({.prompt = "Hello"});

    REQUIRE(result.has_value());
    CHECK(result->text == "done");
    const auto requests = recording.requests();
    REQUIRE(requests.size() == 1);
    REQUIRE(requests.front().system_prompt.has_value());
    CHECK(requests.front().system_prompt->contains("FileRead"));
    CHECK_FALSE(requests.front().system_prompt->contains("MemoryRecall"));
    CHECK_FALSE(requests.front().system_prompt->contains("MemoryRemember"));
    CHECK_FALSE(requests.front().system_prompt->contains("MemoryForget"));
  });
}

TEST_CASE("AgentSession resumes completed history after a failed transcript commit",
          "[integration][bootstrap][memory][atomic]") {
  TempDir temp{"oran-session-atomic-turn"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false, true);
    auto database =
        storage::Connection::open(storage::ConnectionOptions{.path = std::string{assembly.sessions_path()}});
    REQUIRE(database.has_value());
    auto trigger = database->execute(R"sql(
      CREATE TRIGGER reject_turn BEFORE INSERT ON session_messages
      WHEN instr(NEW.content_json, 'reject-turn') > 0
      BEGIN SELECT RAISE(ABORT, 'reject completed turn'); END;
    )sql");
    REQUIRE(trigger.has_value());
    RecordingProvider recording{{
        text_response("saved answer"),
        provider::Response{
            .blocks = {core::ToolUseContent{
                .id = "remember-1",
                .name = "MemoryRemember",
                .input_json = R"({"id":"accepted-note","kind":"project","title":"Note","body":"accepted effect"})"}},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::string{"fake-1"},
            .route_profile_used = std::nullopt,
        },
        text_response("reject-turn"),
        text_response("continued answer"),
    }};
    auto options = base_runner_options(io, assembly, cfg, recording);
    options.mode = permission::Mode::permissive;
    options.session_id.back() = std::byte{0x42};
    auto runner = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(runner.has_value());
    auto first = co_await (*runner)->run_prompt(agent::PromptRequest{.prompt = "saved prompt"});
    REQUIRE(first.has_value());

    auto failed = co_await (*runner)->run_prompt(agent::PromptRequest{.prompt = "write a note"});

    REQUIRE_FALSE(failed.has_value());
    REQUIRE(failed.error().kind() == core::ErrorKind::storage);
    REQUIRE(std::ranges::contains(failed.error().context(),
                                  core::Error::ContextEntry{"sqlite_message", "reject completed turn"}));
    auto loaded =
        co_await assembly.session_store()->load(memory::session::SessionId{.value = "00000000000000000000000000000042"},
                                                memory::session::AgentKey{.value = "coder"});
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->size() == 2);
    REQUIRE((*loaded)[0].blocks == core::Message::user_text("saved prompt").blocks);
    REQUIRE((*loaded)[1].blocks == core::Message::assistant_text("saved answer").blocks);
    auto note = co_await assembly.longterm_memory_backend()->get(
        memory::longterm::RecordKey{.id = "accepted-note", .scope_key = "scope-A"});
    REQUIRE(note.has_value());
    REQUIRE(note->body == "accepted effect");

    auto continued = co_await (*runner)->run_prompt(agent::PromptRequest{.prompt = "continue"});
    REQUIRE(continued.has_value());
    REQUIRE(continued->text == "continued answer");
    const auto requests = recording.requests();
    REQUIRE(requests.size() == 4);
    REQUIRE(requests.back().messages.size() == 3);
    REQUIRE(requests.back().messages[0].blocks == core::Message::user_text("saved prompt").blocks);
    REQUIRE(requests.back().messages[1].blocks == core::Message::assistant_text("saved answer").blocks);
    REQUIRE(requests.back().messages[2].blocks == core::Message::user_text("continue").blocks);
  });
}
