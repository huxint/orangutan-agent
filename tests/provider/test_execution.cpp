// tests/provider/test_execution.cpp — provider execution retry/fallback coverage.

#include <oran/provider.hpp>

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/core/content.hpp>
#include <oran/core/error.hpp>

#include "../test-helpers/run_async.hpp"

namespace {

using namespace std::chrono_literals;

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace prov = orangutan::provider;
namespace test = orangutan::tests;

prov::ModelTarget target(std::string profile, std::string model) {
  return prov::ModelTarget{
      .profile = std::move(profile),
      .model = std::move(model),
      .protocol = prov::ProtocolKind::anthropic_messages,
      .thinking_budget = std::nullopt,
      .cache = std::nullopt,
  };
}

prov::Route route_with_fallback() {
  return prov::Route{
      .primary = target("primary-profile", "primary-model"),
      .fallbacks = {target("fallback-profile", "fallback-model")},
  };
}

prov::Response text_response(std::string text, std::optional<std::string> model_used = std::nullopt) {
  return prov::Response{
      .blocks = {core::TextContent{.text = std::move(text)}},
      .stop_reason = core::StopReason::end_turn,
      .usage = {},
      .model_used = std::move(model_used),
  };
}

prov::Request request_with_retry(std::uint32_t max_attempts, std::chrono::milliseconds backoff = 0ms) {
  auto request = prov::Request{};
  request.retry = prov::RetryPolicy{
      .max_attempts = max_attempts,
      .initial_backoff = backoff,
  };
  return request;
}

std::optional<std::string_view> context_value(const core::Error& error, std::string_view key) {
  const auto it = std::ranges::find_if(error.context(), [&](const auto& entry) { return entry.first == key; });
  if (it == error.context().end()) {
    return std::nullopt;
  }
  return it->second;
}

class RecordingSystem final : public prov::System {
public:
  explicit RecordingSystem(std::vector<core::Result<prov::Response>> plan) : plan_{std::move(plan)} {}

  [[nodiscard]] async::Awaitable<core::Result<prov::Response>>
  send(prov::Request request, prov::Route route, prov::EventSink* sink = nullptr) const override {
    static_cast<void>(request);
    static_cast<void>(sink);
    routes_seen_.push_back(route);

    const auto cursor = cursor_++;
    if (cursor >= plan_.size()) {
      co_return std::unexpected(core::Error::internal("recording provider exhausted"));
    }

    const auto& scripted = plan_[cursor];
    if (scripted.has_value()) {
      co_return *scripted;
    }
    co_return std::unexpected(scripted.error());
  }

  [[nodiscard]] const std::vector<prov::Route>& routes_seen() const noexcept {
    return routes_seen_;
  }

private:
  std::vector<core::Result<prov::Response>> plan_;
  mutable std::vector<prov::Route> routes_seen_;
  mutable std::size_t cursor_{0};
};

class StreamingFailureSystem final : public prov::System {
public:
  [[nodiscard]] async::Awaitable<core::Result<prov::Response>>
  send(prov::Request request, prov::Route route, prov::EventSink* sink = nullptr) const override {
    static_cast<void>(request);
    routes_seen_.push_back(route);
    if (sink != nullptr) {
      sink->on_text_delta("partial");
    }
    co_return std::unexpected(core::Error::network("stream failed after visible output"));
  }

  [[nodiscard]] const std::vector<prov::Route>& routes_seen() const noexcept {
    return routes_seen_;
  }

private:
  mutable std::vector<prov::Route> routes_seen_;
};

class TextSink final : public prov::EventSink {
public:
  void on_text_delta(std::string_view delta) override {
    text.append(delta);
  }

  std::string text;
};

}  // namespace

TEST_CASE("execution runtime retries retryable errors on the same target", "[unit][provider][execution]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    RecordingSystem backend{std::vector<core::Result<prov::Response>>{
        std::unexpected(core::Error::network("temporary network failure")),
        text_response("ok"),
    }};
    prov::execution::Runtime runtime{backend};

    auto result = co_await runtime.send(request_with_retry(2), route_with_fallback(), nullptr);

    REQUIRE(result.has_value());
    REQUIRE(std::get<core::TextContent>(result->blocks.front()).text == "ok");
    REQUIRE(result->model_used == std::string{"primary-model"});
    REQUIRE(backend.routes_seen().size() == 2);
    REQUIRE(backend.routes_seen()[0].primary.model == "primary-model");
    REQUIRE(backend.routes_seen()[1].primary.model == "primary-model");
    REQUIRE(backend.routes_seen()[0].fallbacks.empty());
    REQUIRE(backend.routes_seen()[1].fallbacks.empty());
  });
}

TEST_CASE("execution runtime falls back after retryable primary exhaustion", "[unit][provider][execution]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    RecordingSystem backend{std::vector<core::Result<prov::Response>>{
        std::unexpected(core::Error::network("first primary failure")),
        std::unexpected(core::Error::upstream("second primary failure")),
        text_response("fallback ok"),
    }};
    prov::execution::Runtime runtime{backend};

    auto result = co_await runtime.send(request_with_retry(2), route_with_fallback(), nullptr);

    REQUIRE(result.has_value());
    REQUIRE(std::get<core::TextContent>(result->blocks.front()).text == "fallback ok");
    REQUIRE(result->model_used == std::string{"fallback-model"});
    REQUIRE(backend.routes_seen().size() == 3);
    REQUIRE(backend.routes_seen()[0].primary.model == "primary-model");
    REQUIRE(backend.routes_seen()[1].primary.model == "primary-model");
    REQUIRE(backend.routes_seen()[2].primary.model == "fallback-model");
  });
}

TEST_CASE("execution runtime keeps provider-supplied model_used", "[unit][provider][execution]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    RecordingSystem backend{std::vector<core::Result<prov::Response>>{
        text_response("ok", std::string{"adapter-reported-model"}),
    }};
    prov::execution::Runtime runtime{backend};

    auto result = co_await runtime.send(request_with_retry(1), route_with_fallback(), nullptr);

    REQUIRE(result.has_value());
    REQUIRE(result->model_used == std::string{"adapter-reported-model"});
  });
}

TEST_CASE("execution runtime stops on non-retryable errors", "[unit][provider][execution]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    RecordingSystem backend{std::vector<core::Result<prov::Response>>{
        std::unexpected(core::Error{core::ErrorKind::auth, "bad key"}),
        text_response("should not run"),
    }};
    prov::execution::Runtime runtime{backend};

    auto result = co_await runtime.send(request_with_retry(2), route_with_fallback(), nullptr);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::auth);
    REQUIRE(context_value(result.error(), "provider_profile") == std::optional<std::string_view>{"primary-profile"});
    REQUIRE(context_value(result.error(), "provider_model") == std::optional<std::string_view>{"primary-model"});
    REQUIRE(context_value(result.error(), "attempt") == std::optional<std::string_view>{"1"});
    REQUIRE(backend.routes_seen().size() == 1);
  });
}

TEST_CASE("execution runtime does not retry after visible stream output", "[unit][provider][execution]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    StreamingFailureSystem backend;
    TextSink sink;
    prov::execution::Runtime runtime{backend};

    auto result = co_await runtime.send(request_with_retry(2), route_with_fallback(), &sink);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::network);
    REQUIRE(context_value(result.error(), "retry_skipped") ==
            std::optional<std::string_view>{"stream_already_emitted"});
    REQUIRE(context_value(result.error(), "fallback_skipped") ==
            std::optional<std::string_view>{"stream_already_emitted"});
    REQUIRE(sink.text == "partial");
    REQUIRE(backend.routes_seen().size() == 1);
    REQUIRE(backend.routes_seen()[0].primary.model == "primary-model");
  });
}

TEST_CASE("execution runtime rejects zero retry attempts before sending", "[unit][provider][execution]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    RecordingSystem backend{std::vector<core::Result<prov::Response>>{
        text_response("should not run"),
    }};
    prov::execution::Runtime runtime{backend};

    auto result = co_await runtime.send(request_with_retry(0), route_with_fallback(), nullptr);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(backend.routes_seen().empty());
  });
}

TEST_CASE("execution runtime observes cancellation during retry backoff", "[unit][provider][execution]") {
  asio::io_context io;
  asio::cancellation_signal signal;
  RecordingSystem backend{std::vector<core::Result<prov::Response>>{
      std::unexpected(core::Error::network("temporary network failure")),
      text_response("should not run"),
  }};
  prov::execution::Runtime runtime{backend};

  std::optional<core::Result<prov::Response>> result;
  std::exception_ptr failure;

  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<prov::Response>> {
        co_return co_await runtime.send(request_with_retry(2, 1s), route_with_fallback(), nullptr);
      },
      asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, core::Result<prov::Response> r) {
        failure = ep;
        result = std::move(r);
        io.stop();
      }));

  asio::post(io, [&] { signal.emit(asio::cancellation_type::terminal); });
  io.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
  REQUIRE(context_value(result->error(), "provider_profile") == std::optional<std::string_view>{"primary-profile"});
  REQUIRE(context_value(result->error(), "provider_model") == std::optional<std::string_view>{"primary-model"});
  REQUIRE(context_value(result->error(), "attempt") == std::optional<std::string_view>{"1"});
  REQUIRE(context_value(result->error(), "max_attempts") == std::optional<std::string_view>{"2"});
  REQUIRE(backend.routes_seen().size() == 1);
}

TEST_CASE("execution runtime fills route_profile_used from served target", "[unit][provider][execution]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    SECTION("primary success") {
      RecordingSystem backend{std::vector<core::Result<prov::Response>>{
          text_response("primary ok"),
      }};
      prov::execution::Runtime runtime{backend};

      auto result = co_await runtime.send(request_with_retry(1), route_with_fallback(), nullptr);

      REQUIRE(result.has_value());
      REQUIRE(result->route_profile_used == std::optional<std::string>{"primary-profile"});
    }
    SECTION("fallback success") {
      RecordingSystem backend{std::vector<core::Result<prov::Response>>{
          std::unexpected(core::Error::network("first primary failure")),
          std::unexpected(core::Error::upstream("second primary failure")),
          text_response("fallback ok"),
      }};
      prov::execution::Runtime runtime{backend};

      auto result = co_await runtime.send(request_with_retry(2), route_with_fallback(), nullptr);

      REQUIRE(result.has_value());
      REQUIRE(result->model_used == std::string{"fallback-model"});
      REQUIRE(result->route_profile_used == std::optional<std::string>{"fallback-profile"});
    }
    co_return;
  });
}
