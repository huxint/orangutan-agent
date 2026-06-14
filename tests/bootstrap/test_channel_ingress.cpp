// tests/bootstrap/test_channel_ingress.cpp - config-authored channel registration and routing coverage.

#include <chrono>
#include <cstdlib>
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
#include <oran/storage.hpp>

#include "../test-helpers/run_async.hpp"

#if defined(ORAN_ENABLE_CHANNEL_QQ)
#include "../channel-qq/scripted_http_server.hpp"
#include "../test-helpers/ws_test_server.hpp"
#endif

namespace async = orangutan::async;
namespace bootstrap = orangutan::bootstrap;
namespace channel = orangutan::channel;
namespace config = orangutan::config;
namespace core = orangutan::core;
namespace provider = orangutan::provider;
namespace storage = orangutan::storage;
namespace test = orangutan::tests;
#if defined(ORAN_ENABLE_CHANNEL_QQ)
namespace ws = orangutan::tests::ws;
#endif

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

class ScopedEnv {
public:
  ScopedEnv(std::string name, std::string value) : name_{std::move(name)} {
    if (const auto* old = std::getenv(name_.c_str()); old != nullptr) {
      old_value_ = old;
    }
    setenv(name_.c_str(), value.c_str(), 1);
  }

  ~ScopedEnv() {
    if (old_value_) {
      setenv(name_.c_str(), old_value_->c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
  }

  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
  std::string name_;
  std::optional<std::string> old_value_;
};

class ScopedUnsetEnv {
public:
  explicit ScopedUnsetEnv(std::string name) : name_{std::move(name)} {
    if (const auto* old = std::getenv(name_.c_str()); old != nullptr) {
      old_value_ = old;
      unsetenv(name_.c_str());
    }
  }

  ~ScopedUnsetEnv() {
    if (old_value_) {
      setenv(name_.c_str(), old_value_->c_str(), 1);
    }
  }

  ScopedUnsetEnv(const ScopedUnsetEnv&) = delete;
  ScopedUnsetEnv& operator=(const ScopedUnsetEnv&) = delete;

private:
  std::string name_;
  std::optional<std::string> old_value_;
};

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

channel::InboundMessage inbound(std::string text, std::string conversation_id = "room-1") {
  return channel::InboundMessage{
      .channel_id = {},
      .conversation_id = std::move(conversation_id),
      .user_id = "user-1",
      .display_name = "Operator",
      .content = {core::TextContent{.text = std::move(text)}},
      .replies_to = {},
      .received_at = core::Time::epoch(),
  };
}

channel::ChannelPromptRunRequest
prompt_request(std::string prompt, std::string channel_id, std::string conversation_id = "room-1") {
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

constexpr std::string_view kTwoChannelConfig = R"json({
  "channels": [
    {"id": "mock-a", "kind": "mock", "agent_key": "concierge", "inbound_capacity": 8},
    {"id": "mock-b", "kind": "mock"}
  ],
  "agents": {
    "concierge": {
      "prompt_overlay": "Agent overlay: answer like a concierge."
    }
  }
})json";

#if defined(ORAN_ENABLE_CHANNEL_QQ)
[[nodiscard]] test::ScriptedResponse qq_token_ok(std::string token) {
  return test::ScriptedResponse{
      .status = 200,
      .body = R"({"access_token":")" + std::move(token) + R"(","expires_in":7200})",
      .headers = {{"Content-Type", "application/json"}},
      .delay = std::chrono::milliseconds{0},
  };
}

[[nodiscard]] test::ScriptedResponse qq_api_response(int status, std::string body) {
  return test::ScriptedResponse{
      .status = status,
      .body = std::move(body),
      .headers = {{"Content-Type", "application/json"}},
      .delay = std::chrono::milliseconds{0},
  };
}

[[nodiscard]] std::string qq_hello_frame() {
  return R"({"op":10,"d":{"heartbeat_interval":200}})";
}

[[nodiscard]] std::string qq_ready_frame() {
  return R"({"op":0,"s":1,"t":"READY","d":{"session_id":"sess-round-trip"}})";
}

[[nodiscard]] std::string qq_c2c_message_frame() {
  return R"({"op":0,"s":2,"t":"C2C_MESSAGE_CREATE","d":{"id":"msg-round-trip","content":"ping from qq","author":{"user_openid":"user-openid","username":"QQ Operator"}}})";
}
#endif

}  // namespace

TEST_CASE("register_configured_channels registers mock adapters and reports skipped kinds",
          "[unit][bootstrap][channel_ingress]") {
  asio::io_context io;
  auto cfg = parse_config(R"json({
  "channels": [
    {"id": "mock-main", "kind": "mock", "inbound_capacity": 1},
    {"id": "discord-main", "kind": "discord"}
  ]
})json");

  channel::ChannelManager manager{io.get_executor()};
  auto report = bootstrap::register_configured_channels(manager, io.get_executor(), cfg);

  REQUIRE(report.has_value());
  REQUIRE(report->registered_count == 1);
  REQUIRE(manager.registered_count() == 1);
  REQUIRE(manager.contains("mock-main"));
  REQUIRE_FALSE(manager.contains("discord-main"));

  REQUIRE(report->skipped.size() == 1);
  REQUIRE(report->skipped[0].id == "discord-main");
  REQUIRE(report->skipped[0].kind == "discord");

  REQUIRE(report->mocks.size() == 1);
  REQUIRE(report->mocks[0].id == "mock-main");
  REQUIRE(report->mocks[0].mock != nullptr);

  // The configured inbound capacity bounds the adapter queue.
  REQUIRE(report->mocks[0].mock->push_inbound(inbound("fits")).has_value());
  auto overflow = report->mocks[0].mock->push_inbound(inbound("dropped"));
  REQUIRE_FALSE(overflow.has_value());
  REQUIRE(overflow.error().kind() == core::ErrorKind::mailbox_overflowed);
}

#if !defined(ORAN_ENABLE_CHANNEL_QQ)
TEST_CASE("register_configured_channels skips QQ adapters when the option is disabled",
          "[unit][bootstrap][channel_ingress][channel-qq]") {
  asio::io_context io;
  auto cfg = parse_config(R"json({
  "channels": [
    {
      "id": "qq-main",
      "kind": "qq",
      "qq_app_id_env": "ORAN_TEST_QQ_APP_ID",
      "qq_client_secret_env": "ORAN_TEST_QQ_CLIENT_SECRET",
      "qq_gateway_url": "wss://127.0.0.1/gateway"
    }
  ]
})json");

  channel::ChannelManager manager{io.get_executor()};
  auto report = bootstrap::register_configured_channels(manager, io.get_executor(), cfg);

  REQUIRE(report.has_value());
  REQUIRE(report->registered_count == 0);
  REQUIRE(manager.registered_count() == 0);
  REQUIRE_FALSE(manager.contains("qq-main"));
  REQUIRE(report->skipped.size() == 1);
  REQUIRE(report->skipped[0].id == "qq-main");
  REQUIRE(report->skipped[0].kind == "qq");
}
#else
TEST_CASE("register_configured_channels registers QQ adapters when the option is enabled",
          "[unit][bootstrap][channel_ingress][channel-qq]") {
  ScopedEnv app_id{"ORAN_TEST_QQ_APP_ID", "app-123"};
  ScopedEnv client_secret{"ORAN_TEST_QQ_CLIENT_SECRET", "secret-123"};

  asio::io_context io;
  auto cfg = parse_config(R"json({
  "channels": [
    {
      "id": "qq-main",
      "kind": "qq",
      "qq_app_id_env": "ORAN_TEST_QQ_APP_ID",
      "qq_client_secret_env": "ORAN_TEST_QQ_CLIENT_SECRET",
      "qq_token_url": "http://127.0.0.1/token",
      "qq_api_base_url": "http://127.0.0.1/api",
      "qq_gateway_url": "wss://127.0.0.1/gateway"
    }
  ]
})json");

  channel::ChannelManager manager{io.get_executor()};
  auto report = bootstrap::register_configured_channels(manager, io.get_executor(), cfg);

  REQUIRE(report.has_value());
  REQUIRE(report->registered_count == 1);
  REQUIRE(report->skipped.empty());
  REQUIRE(report->mocks.empty());
  REQUIRE(manager.registered_count() == 1);
  REQUIRE(manager.contains("qq-main"));
  auto caps = manager.caps("qq-main");
  REQUIRE(caps.has_value());
  REQUIRE(caps->mentions);
  REQUIRE(caps->reply_quoting);
  REQUIRE(caps->max_text_bytes == 5'000);
}

TEST_CASE("register_configured_channels fails closed when QQ credential env vars are missing",
          "[unit][bootstrap][channel_ingress][channel-qq]") {
  ScopedUnsetEnv app_id{"ORAN_TEST_QQ_MISSING_APP_ID"};
  ScopedUnsetEnv client_secret{"ORAN_TEST_QQ_MISSING_CLIENT_SECRET"};

  asio::io_context io;
  auto cfg = parse_config(R"json({
  "channels": [
    {
      "id": "qq-main",
      "kind": "qq",
      "qq_app_id_env": "ORAN_TEST_QQ_MISSING_APP_ID",
      "qq_client_secret_env": "ORAN_TEST_QQ_MISSING_CLIENT_SECRET",
      "qq_gateway_url": "wss://127.0.0.1/gateway"
    }
  ]
})json");

  channel::ChannelManager manager{io.get_executor()};
  auto report = bootstrap::register_configured_channels(manager, io.get_executor(), cfg);

  REQUIRE_FALSE(report.has_value());
  REQUIRE(report.error().kind() == core::ErrorKind::auth);
  REQUIRE(manager.registered_count() == 0);
}

TEST_CASE("registered QQ channels round-trip one gateway message through the routed prompt path",
          "[unit][bootstrap][channel_ingress][channel-qq][async]") {
  ScopedEnv app_id{"ORAN_TEST_QQ_ROUND_TRIP_APP_ID", "app-round-trip"};
  ScopedEnv client_secret{"ORAN_TEST_QQ_ROUND_TRIP_CLIENT_SECRET", "secret-round-trip"};
  test::ScriptedHttpServer qq_api_server{{qq_token_ok("tok-round-trip"), qq_api_response(200, R"({"id":"out-qq-1"})")}};
  ws::ScriptedWsServer gateway_server{{{
      ws::SendText{qq_hello_frame()},
      ws::RecvFrame{},
      ws::SendText{qq_ready_frame()},
      ws::SendText{qq_c2c_message_frame()},
  }}};
  TempDir temp{"oran-bootstrap-channel-qq-round-trip"};
  auto config_json = std::string{R"json({
  "channels": [
    {
      "id": "qq-main",
      "kind": "qq",
      "agent_key": "concierge",
      "qq_app_id_env": "ORAN_TEST_QQ_ROUND_TRIP_APP_ID",
      "qq_client_secret_env": "ORAN_TEST_QQ_ROUND_TRIP_CLIENT_SECRET",
      "qq_token_url": ")json"} +
                     qq_api_server.url("/app/getAppAccessToken") + R"json(",
      "qq_api_base_url": ")json" +
                     qq_api_server.base_url() + R"json(",
      "qq_gateway_url": ")json" +
                     gateway_server.url() + R"json("
    }
  ],
  "agents": {
    "concierge": {
      "prompt_overlay": "Agent overlay: answer like a concierge."
    }
  }
})json";
  auto cfg = parse_config(config_json);

  test::run_async(
      [&temp, &cfg](asio::io_context& io) -> async::Awaitable<void> {
        auto assembly_options = bootstrap::RuntimeAssemblyOptions{};
        assembly_options.audit_enabled = true;
        assembly_options.session_memory_enabled = false;
        assembly_options.longterm_memory_enabled = false;
        auto built = bootstrap::RuntimeAssembly::build(temp.path().string(), io.get_executor(), assembly_options);
        REQUIRE(built.has_value());
        auto assembly = std::move(*built);
        REQUIRE(assembly.audit_enabled());
        REQUIRE(assembly.trace_repository() != nullptr);

        RecordingProvider recording{{text_response("round trip answer")}};
        channel::ChannelManager manager{io.get_executor()};

        auto report = bootstrap::register_configured_channels(manager, io.get_executor(), cfg);
        REQUIRE(report.has_value());
        REQUIRE(report->registered_count == 1);
        REQUIRE(report->skipped.empty());
        REQUIRE(manager.contains("qq-main"));

        auto routed = bootstrap::make_routed_channel_prompt_runner(base_bridge_options(io, assembly, cfg, recording));
        REQUIRE(routed.has_value());

        auto started = co_await manager.start_all();
        REQUIRE(started.has_value());

        auto received = co_await manager.receive_one("qq-main");
        REQUIRE(received.has_value());

        auto receipt = co_await channel::dispatch_one(manager, *routed);
        REQUIRE(receipt.has_value());
        REQUIRE(receipt->message_id == "out-qq-1");

        const auto requests = recording.requests();
        REQUIRE(requests.size() == 1);
        REQUIRE(requests.front().messages.size() == 1);
        REQUIRE(requests.front().messages.front().blocks == core::Message::user_text("ping from qq").blocks);
        REQUIRE(requests.front().system_prompt.has_value());
        REQUIRE(requests.front().system_prompt->contains("Agent overlay: answer like a concierge."));

        auto traces = co_await assembly.trace_repository()->list_turns(
            storage::ListTraceTurnsOptions{.agent_key = "concierge", .limit = 10});
        REQUIRE(traces.has_value());
        REQUIRE(traces->size() == 1);
        REQUIRE(traces->front().agent_key == "concierge");
      },
      std::chrono::seconds{5});

  REQUIRE(std::filesystem::exists(temp.path() / ".orangutan" / "audit.db"));
  REQUIRE(gateway_server.accepted_count() == 1);
  const auto frames = gateway_server.recorded_frames();
  REQUIRE(frames.size() == 1);
  REQUIRE(frames.front().payload.contains(R"("op":2)"));
  REQUIRE(frames.front().payload.contains("tok-round-trip"));

  REQUIRE(qq_api_server.served_count() == 2);
  const auto reply_request = qq_api_server.request_text(1);
  REQUIRE(reply_request.starts_with("POST /v2/users/user-openid/messages HTTP/1.1"));
  REQUIRE(reply_request.contains("Authorization: QQBot tok-round-trip"));
  REQUIRE(reply_request.contains(R"("content":"round trip answer")"));
  REQUIRE(reply_request.contains(R"("msg_id":"msg-round-trip")"));
  REQUIRE(reply_request.contains(R"("msg_seq":1)"));
}
#endif

TEST_CASE("register_configured_channels handles empty channel config", "[unit][bootstrap][channel_ingress]") {
  asio::io_context io;
  auto cfg = parse_config(R"json({})json");

  channel::ChannelManager manager{io.get_executor()};
  auto report = bootstrap::register_configured_channels(manager, io.get_executor(), cfg);

  REQUIRE(report.has_value());
  REQUIRE(report->registered_count == 0);
  REQUIRE(report->skipped.empty());
  REQUIRE(report->mocks.empty());
}

TEST_CASE("make_routed_channel_prompt_runner routes requests to each channel's configured agent",
          "[unit][bootstrap][channel_ingress][agent]") {
  TempDir temp{"oran-bootstrap-channel-ingress-routing"};
  auto cfg = parse_config(kTwoChannelConfig);

  test::run_async([&temp, &cfg](asio::io_context& io) -> async::Awaitable<void> {
    auto assembly = build_assembly(temp.path(), io);
    RecordingProvider recording{{text_response("concierge answer"), text_response("default answer")}};

    auto routed = bootstrap::make_routed_channel_prompt_runner(base_bridge_options(io, assembly, cfg, recording));
    REQUIRE(routed.has_value());

    auto first = co_await (*routed)(prompt_request("greet the guest", "mock-a"));
    REQUIRE(first.has_value());
    REQUIRE(first->text == "concierge answer");

    auto second = co_await (*routed)(prompt_request("plain question", "mock-b"));
    REQUIRE(second.has_value());
    REQUIRE(second->text == "default answer");

    const auto requests = recording.requests();
    REQUIRE(requests.size() == 2);
    REQUIRE(requests[0].system_prompt.has_value());
    REQUIRE(requests[0].system_prompt->contains("Agent overlay: answer like a concierge."));
    REQUIRE(requests[1].system_prompt.has_value());
    REQUIRE_FALSE(requests[1].system_prompt->contains("Agent overlay: answer like a concierge."));

    auto unknown = co_await (*routed)(prompt_request("hello", "mock-unknown"));
    REQUIRE_FALSE(unknown.has_value());
    REQUIRE(unknown.error().kind() == core::ErrorKind::not_found);
  });
}

TEST_CASE("make_routed_channel_prompt_runner keeps per-channel conversation sessions independent",
          "[unit][bootstrap][channel_ingress][memory]") {
  TempDir temp{"oran-bootstrap-channel-ingress-sessions"};
  auto cfg = parse_config(R"json({
  "channels": [
    {"id": "mock-a", "kind": "mock"},
    {"id": "mock-b", "kind": "mock"}
  ]
})json");

  test::run_async([&temp, &cfg](asio::io_context& io) -> async::Awaitable<void> {
    auto assembly = build_assembly(temp.path(), io, true);
    RecordingProvider recording{
        {text_response("first answer"), text_response("second answer"), text_response("other channel answer")}};

    auto routed = bootstrap::make_routed_channel_prompt_runner(base_bridge_options(io, assembly, cfg, recording));
    REQUIRE(routed.has_value());

    auto first = co_await (*routed)(prompt_request("first prompt", "mock-a"));
    REQUIRE(first.has_value());
    auto second = co_await (*routed)(prompt_request("second prompt", "mock-a"));
    REQUIRE(second.has_value());
    auto other = co_await (*routed)(prompt_request("other prompt", "mock-b"));
    REQUIRE(other.has_value());

    const auto requests = recording.requests();
    REQUIRE(requests.size() == 3);
    REQUIRE(requests[0].messages.size() == 1);
    REQUIRE(requests[1].messages.size() == 3);
    REQUIRE(requests[1].messages[2].blocks == core::Message::user_text("second prompt").blocks);
    // Same conversation id on a different channel starts its own session.
    REQUIRE(requests[2].messages.size() == 1);
    REQUIRE(requests[2].messages[0].blocks == core::Message::user_text("other prompt").blocks);
  });
}

TEST_CASE("make_routed_channel_prompt_runner validates configuration", "[unit][bootstrap][channel_ingress]") {
  TempDir temp{"oran-bootstrap-channel-ingress-validate"};
  asio::io_context io;
  auto assembly = build_assembly(temp.path(), io);
  RecordingProvider recording{{}};

  SECTION("no channels configured") {
    auto cfg = parse_config(R"json({})json");
    auto routed = bootstrap::make_routed_channel_prompt_runner(base_bridge_options(io, assembly, cfg, recording));
    REQUIRE_FALSE(routed.has_value());
    REQUIRE(routed.error().kind() == core::ErrorKind::invalid_argument);
  }

  SECTION("missing config") {
    auto cfg = parse_config(R"json({"channels": [{"id": "mock-a", "kind": "mock"}]})json");
    auto options = base_bridge_options(io, assembly, cfg, recording);
    options.config = nullptr;
    auto routed = bootstrap::make_routed_channel_prompt_runner(std::move(options));
    REQUIRE_FALSE(routed.has_value());
    REQUIRE(routed.error().kind() == core::ErrorKind::invalid_argument);
  }
}

TEST_CASE("configured channels route external messages to their agents end-to-end",
          "[unit][bootstrap][channel_ingress][dispatch]") {
  TempDir temp{"oran-bootstrap-channel-ingress-e2e"};
  auto cfg = parse_config(kTwoChannelConfig);

  test::run_async([&temp, &cfg](asio::io_context& io) -> async::Awaitable<void> {
    auto assembly = build_assembly(temp.path(), io);
    RecordingProvider recording{{text_response("concierge reply"), text_response("default reply")}};

    channel::ChannelManager manager{io.get_executor()};
    auto report = bootstrap::register_configured_channels(manager, io.get_executor(), cfg);
    REQUIRE(report.has_value());
    REQUIRE(report->registered_count == 2);
    REQUIRE(report->mocks.size() == 2);
    auto* mock_a = report->mocks[0].mock;
    auto* mock_b = report->mocks[1].mock;

    auto routed = bootstrap::make_routed_channel_prompt_runner(base_bridge_options(io, assembly, cfg, recording));
    REQUIRE(routed.has_value());

    auto started = co_await manager.start_all();
    REQUIRE(started.has_value());

    REQUIRE(mock_a->push_inbound(inbound("hello concierge")).has_value());
    auto pumped_a = co_await manager.receive_one("mock-a");
    REQUIRE(pumped_a.has_value());
    auto receipt_a = co_await channel::dispatch_one(manager, *routed);
    REQUIRE(receipt_a.has_value());

    REQUIRE(mock_b->push_inbound(inbound("hello default", "room-2")).has_value());
    auto pumped_b = co_await manager.receive_one("mock-b");
    REQUIRE(pumped_b.has_value());
    auto receipt_b = co_await channel::dispatch_one(manager, *routed);
    REQUIRE(receipt_b.has_value());

    const auto requests = recording.requests();
    REQUIRE(requests.size() == 2);
    REQUIRE(requests[0].messages[0].blocks == core::Message::user_text("hello concierge").blocks);
    REQUIRE(requests[0].system_prompt->contains("Agent overlay: answer like a concierge."));
    REQUIRE(requests[1].messages[0].blocks == core::Message::user_text("hello default").blocks);

    REQUIRE(mock_a->sent_messages().size() == 1);
    REQUIRE(core::text_view(mock_a->sent_messages().front().content.front()) == "concierge reply");
    REQUIRE(mock_a->sent_messages().front().conversation_id == "room-1");
    REQUIRE(mock_b->sent_messages().size() == 1);
    REQUIRE(core::text_view(mock_b->sent_messages().front().content.front()) == "default reply");
    REQUIRE(mock_b->sent_messages().front().conversation_id == "room-2");
  });
}
