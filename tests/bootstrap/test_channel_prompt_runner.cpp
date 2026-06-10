// tests/bootstrap/test_channel_prompt_runner.cpp - bootstrap channel prompt bridge coverage.

#include <chrono>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/bootstrap.hpp>
#include <oran/channel.hpp>
#include <oran/config.hpp>
#include <oran/core/content.hpp>
#include <oran/core/error.hpp>
#include <oran/core/message.hpp>
#include <oran/core/stop_reason.hpp>
#include <oran/provider.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace bootstrap = orangutan::bootstrap;
namespace channel = orangutan::channel;
namespace config = orangutan::config;
namespace core = orangutan::core;
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

bootstrap::RuntimeAssembly
build_assembly(const std::filesystem::path& workspace, asio::io_context& io, bool session_memory_enabled = false) {
  auto options = bootstrap::RuntimeAssemblyOptions{};
  options.audit_enabled = false;
  options.session_memory_enabled = session_memory_enabled;
  options.longterm_memory_enabled = false;
  auto assembly = bootstrap::RuntimeAssembly::build(workspace.string(), io.get_executor(), std::move(options));
  REQUIRE(assembly.has_value());
  return std::move(*assembly);
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

bootstrap::ChannelAgentPromptRunnerOptions base_bridge_options(asio::io_context& io,
                                                               bootstrap::RuntimeAssembly& assembly,
                                                               config::Config& cfg,
                                                               provider::System& provider_system) {
  auto options = bootstrap::ChannelAgentPromptRunnerOptions{};
  options.executor = io.get_executor();
  options.assembly = &assembly;
  options.config = &cfg;
  options.provider = &provider_system;
  options.route = test_route();
  options.max_tokens = 1024;
  return options;
}

channel::ChannelPromptRunRequest
prompt_request(std::string prompt, std::string conversation_id = "room-1", std::string channel_id = "mock-main") {
  return channel::ChannelPromptRunRequest{
      .channel_id = std::move(channel_id),
      .conversation_id = std::move(conversation_id),
      .user_id = "user-1",
      .display_name = "Operator",
      .prompt = std::move(prompt),
      .origin = {.kind = "channel", .source = "mock"},
      .caps = {},
      .received_at = core::Time::epoch(),
  };
}

}  // namespace

TEST_CASE("channel prompt bridge hands one mock ingress message to the agent path and replies",
          "[unit][bootstrap][channel_prompt_runner][dispatch]") {
  TempDir temp{"oran-bootstrap-channel-prompt-runner-dispatch"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io);
    RecordingProvider recording{{text_response("answer from the agent")}};

    auto bridge = bootstrap::make_channel_agent_prompt_runner(base_bridge_options(io, assembly, cfg, recording));
    REQUIRE(bridge.has_value());

    auto adapter = std::make_unique<channel::MockChannel>(io.get_executor());
    auto* mock = adapter.get();
    channel::ChannelManager manager{io.get_executor()};
    REQUIRE(manager.register_adapter(std::move(adapter)).has_value());
    auto started = co_await manager.start_all();
    REQUIRE(started.has_value());

    REQUIRE(mock->push_inbound(channel::InboundMessage{
                                   .channel_id = {},
                                   .conversation_id = "room-1",
                                   .user_id = "user-1",
                                   .display_name = "Operator",
                                   .content = {core::TextContent{.text = "hello agent"}},
                                   .replies_to = {},
                                   .received_at = core::Time::epoch(),
                               })
                .has_value());
    auto pumped = co_await manager.receive_one("mock-main");
    REQUIRE(pumped.has_value());

    auto receipt = co_await channel::dispatch_one(manager, *bridge);

    REQUIRE(receipt.has_value());
    REQUIRE(receipt->message_id == "mock-main-1");

    const auto requests = recording.requests();
    REQUIRE(requests.size() == 1);
    REQUIRE(requests[0].messages.size() == 1);
    REQUIRE(requests[0].messages[0].blocks == core::Message::user_text("hello agent").blocks);

    const auto sent = mock->sent_messages();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent.front().conversation_id == "room-1");
    REQUIRE(core::text_view(sent.front().content.front()) == "answer from the agent");
  });
}

TEST_CASE("channel prompt bridge keeps one session per conversation",
          "[unit][bootstrap][channel_prompt_runner][memory]") {
  TempDir temp{"oran-bootstrap-channel-prompt-runner-session"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, true);
    RecordingProvider recording{
        {text_response("first answer"), text_response("second answer"), text_response("other answer")}};

    auto bridge = bootstrap::make_channel_agent_prompt_runner(base_bridge_options(io, assembly, cfg, recording));
    REQUIRE(bridge.has_value());

    auto first = co_await (*bridge)(prompt_request("first prompt"));
    REQUIRE(first.has_value());
    REQUIRE(first->text == "first answer");

    auto second = co_await (*bridge)(prompt_request("second prompt"));
    REQUIRE(second.has_value());
    REQUIRE(second->text == "second answer");

    auto other = co_await (*bridge)(prompt_request("other prompt", "room-2"));
    REQUIRE(other.has_value());
    REQUIRE(other->text == "other answer");

    const auto requests = recording.requests();
    REQUIRE(requests.size() == 3);
    REQUIRE(requests[0].messages.size() == 1);
    REQUIRE(requests[0].messages[0].blocks == core::Message::user_text("first prompt").blocks);
    REQUIRE(requests[1].messages.size() == 3);
    REQUIRE(requests[1].messages[0].blocks == core::Message::user_text("first prompt").blocks);
    REQUIRE(requests[1].messages[1].blocks == core::Message::assistant_text("first answer").blocks);
    REQUIRE(requests[1].messages[2].blocks == core::Message::user_text("second prompt").blocks);
    REQUIRE(requests[2].messages.size() == 1);
    REQUIRE(requests[2].messages[0].blocks == core::Message::user_text("other prompt").blocks);
  });
}

TEST_CASE("channel prompt bridge applies per-agent overlays only when configured",
          "[unit][bootstrap][channel_prompt_runner][agent]") {
  TempDir temp{"oran-bootstrap-channel-prompt-runner-agent-overlay"};
  auto cfg = parse_config(R"json(
{
  "agents": {
    "concierge": {
      "prompt_overlay": "Agent overlay: answer like a concierge."
    }
  }
}
)json");

  test::run_async([&temp, &cfg](asio::io_context& io) -> async::Awaitable<void> {
    auto assembly = build_assembly(temp.path(), io);
    RecordingProvider recording{{text_response("concierge done"), text_response("ghost done")}};

    auto concierge_options = base_bridge_options(io, assembly, cfg, recording);
    concierge_options.agent_key = "concierge";
    auto concierge = bootstrap::make_channel_agent_prompt_runner(std::move(concierge_options));
    REQUIRE(concierge.has_value());

    auto ghost_options = base_bridge_options(io, assembly, cfg, recording);
    ghost_options.agent_key = "ghost";
    auto ghost = bootstrap::make_channel_agent_prompt_runner(std::move(ghost_options));
    REQUIRE(ghost.has_value());

    auto first = co_await (*concierge)(prompt_request("greet the guest"));
    REQUIRE(first.has_value());
    auto second = co_await (*ghost)(prompt_request("greet the guest"));
    REQUIRE(second.has_value());

    const auto requests = recording.requests();
    REQUIRE(requests.size() == 2);
    REQUIRE(requests[0].system_prompt.has_value());
    REQUIRE(requests[1].system_prompt.has_value());
    REQUIRE(requests[0].system_prompt->contains("Agent overlay: answer like a concierge."));
    REQUIRE_FALSE(requests[1].system_prompt->contains("Agent overlay: answer like a concierge."));
  });
}

TEST_CASE("channel prompt bridge validates options", "[unit][bootstrap][channel_prompt_runner]") {
  TempDir temp{"oran-bootstrap-channel-prompt-runner-options"};
  asio::io_context io;
  auto cfg = config::Config{};
  auto assembly = build_assembly(temp.path(), io);
  RecordingProvider recording{{}};

  SECTION("missing assembly") {
    auto options = base_bridge_options(io, assembly, cfg, recording);
    options.assembly = nullptr;
    auto bridge = bootstrap::make_channel_agent_prompt_runner(std::move(options));
    REQUIRE_FALSE(bridge.has_value());
    REQUIRE(bridge.error().kind() == core::ErrorKind::invalid_argument);
  }

  SECTION("missing provider") {
    auto options = base_bridge_options(io, assembly, cfg, recording);
    options.provider = nullptr;
    auto bridge = bootstrap::make_channel_agent_prompt_runner(std::move(options));
    REQUIRE_FALSE(bridge.has_value());
  }

  SECTION("empty route") {
    auto options = base_bridge_options(io, assembly, cfg, recording);
    options.route = provider::Route{};
    auto bridge = bootstrap::make_channel_agent_prompt_runner(std::move(options));
    REQUIRE_FALSE(bridge.has_value());
  }

  SECTION("empty agent key") {
    auto options = base_bridge_options(io, assembly, cfg, recording);
    options.agent_key.clear();
    auto bridge = bootstrap::make_channel_agent_prompt_runner(std::move(options));
    REQUIRE_FALSE(bridge.has_value());
  }
}

TEST_CASE("channel prompt bridge rejects incomplete requests", "[unit][bootstrap][channel_prompt_runner]") {
  TempDir temp{"oran-bootstrap-channel-prompt-runner-request"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io);
    RecordingProvider recording{{}};

    auto bridge = bootstrap::make_channel_agent_prompt_runner(base_bridge_options(io, assembly, cfg, recording));
    REQUIRE(bridge.has_value());

    auto no_prompt = co_await (*bridge)(prompt_request(""));
    REQUIRE_FALSE(no_prompt.has_value());
    REQUIRE(no_prompt.error().kind() == core::ErrorKind::invalid_argument);

    auto no_channel = co_await (*bridge)(prompt_request("hi", "room-1", ""));
    REQUIRE_FALSE(no_channel.has_value());
    REQUIRE(no_channel.error().kind() == core::ErrorKind::invalid_argument);

    auto request = prompt_request("hi");
    request.conversation_id.clear();
    auto no_conversation = co_await (*bridge)(std::move(request));
    REQUIRE_FALSE(no_conversation.has_value());
    REQUIRE(no_conversation.error().kind() == core::ErrorKind::invalid_argument);

    REQUIRE(recording.requests().empty());
  });
}
