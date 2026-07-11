// tests/bootstrap/test_serve.cpp — `--serve` long-lived service coroutine coverage.
//
// The watcher cases exercise `bootstrap::serve_run` directly with a plain
// io_context and an `asio::cancellation_signal`, mirroring the
// watcher-cancellation idiom in tests/io/test_file.cpp. The automation cases
// drive `bootstrap::serve_automation` over a real `AutomationRuntime` on a temp
// database with fake handlers, racing it against a timed sleep to emulate the
// stop signal. Both keep the lifecycle deterministic and avoid raising real
// process signals: the signal -> 128+signum translation is covered by
// tests/bootstrap/test_signal_drain.cpp's shared `signum_from_error` seam.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/streambuf.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/agent.hpp>
#include <oran/async.hpp>
#include <oran/automation.hpp>
#include <oran/bootstrap/serve.hpp>
#include <oran/channel.hpp>
#include <oran/core/capability.hpp>
#include <oran/core/content.hpp>
#include <oran/core/error.hpp>
#include <oran/core/time.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/permission.hpp>
#include <oran/tool.hpp>

#include "../test-helpers/run_async.hpp"

namespace agent = orangutan::agent;
namespace async = orangutan::async;
namespace automation = orangutan::automation;
namespace bootstrap = orangutan::bootstrap;
namespace channel = orangutan::channel;
namespace core = orangutan::core;
namespace permission = orangutan::permission;
namespace test = orangutan::tests;
namespace tool = orangutan::tool;

namespace {

using namespace std::chrono_literals;

/// RAII temp directory (local copy of the pattern used across the bootstrap
/// tests; the buckets are separate TUs with no shared helper for it).
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

struct DriveResult {
  core::Result<void> outcome{};
  std::chrono::steady_clock::duration elapsed{};
};

/// Run `serve_run` until a terminal cancellation is emitted after `cancel_after`,
/// with a hard safety timeout so a hang fails (rather than blocks) the suite.
[[nodiscard]] DriveResult drive_serve_run(bootstrap::ServeOptions options, std::chrono::milliseconds cancel_after) {
  asio::io_context context;
  asio::cancellation_signal signal;
  std::optional<core::Result<void>> result;
  std::exception_ptr failure;

  asio::steady_timer cancel{context};
  cancel.expires_after(cancel_after);
  cancel.async_wait([&](const asio::error_code& ec) {
    if (!ec) {
      signal.emit(asio::cancellation_type::terminal);
    }
  });

  asio::steady_timer timeout{context};
  timeout.expires_after(5s);
  timeout.async_wait([&](const asio::error_code& ec) {
    if (!ec) {
      context.stop();
    }
  });

  const auto started_at = std::chrono::steady_clock::now();
  asio::co_spawn(context,
                 bootstrap::serve_run(context.get_executor(), std::move(options)),
                 asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, core::Result<void> r) {
                   failure = ep;
                   result = std::move(r);
                   timeout.cancel();
                   context.stop();
                 }));

  context.run();
  const auto elapsed = std::chrono::steady_clock::now() - started_at;

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  return DriveResult{.outcome = std::move(*result), .elapsed = elapsed};
}

}  // namespace

TEST_CASE("serve_run idles until cancelled, then stops gracefully", "[unit][bootstrap][serve]") {
  // Watcher disabled: the body is purely the idle wait, so the only way to
  // finish is the cancellation — which also proves it does not return early.
  auto driven = drive_serve_run(bootstrap::ServeOptions{.watch_root = "", .watch_enabled = false}, 50ms);
  REQUIRE(driven.outcome.has_value());
  REQUIRE(driven.elapsed >= 40ms);
}

TEST_CASE("serve_run treats an empty watch root as disabled", "[unit][bootstrap][serve]") {
  auto driven = drive_serve_run(bootstrap::ServeOptions{.watch_root = "", .watch_enabled = true}, 25ms);
  REQUIRE(driven.outcome.has_value());
}

TEST_CASE("serve_run runs the file-view watcher and stops gracefully on cancel", "[unit][bootstrap][serve]") {
  TempDir temp{"oran-serve-watch"};
  auto driven =
      drive_serve_run(bootstrap::ServeOptions{.watch_root = temp.path().string(), .watch_enabled = true}, 30ms);
  REQUIRE(driven.outcome.has_value());
}

TEST_CASE("serve_run keeps serving when the watcher cannot start", "[unit][bootstrap][serve]") {
  // A non-existent root makes the watcher fail to initialize; the service must
  // degrade (report once) and keep idling until cancelled rather than abort.
  const auto bogus = (std::filesystem::temp_directory_path() / "oran-serve-does-not-exist-xyzzy").string();
  auto driven = drive_serve_run(bootstrap::ServeOptions{.watch_root = bogus, .watch_enabled = true}, 30ms);
  REQUIRE(driven.outcome.has_value());
}

namespace {

/// Upsert an every-minute cron job anchored at `now + offset`. A negative
/// offset makes the job due at the current UTC minute; a positive one keeps it
/// in the future. The handler prompt is required but unused by the fake
/// handlers below.
[[nodiscard]] async::Awaitable<core::Result<automation::CronJobRecord>>
seed_cron_job(automation::AutomationRuntime& runtime, std::string job_key, std::chrono::seconds offset) {
  const auto anchor = core::Time{core::time::now_utc().to_system_time_point() + offset};
  co_return co_await runtime.repository().upsert_cron_job(automation::UpsertCronJobRequest{
      .job_key = std::move(job_key),
      .agent_key = "automation",
      .agent_prompt = "tick",
      .schedule = automation::CronSchedule{.expression = "* * * * *", .first_fire_at = anchor},
      .state = {},
  });
}

[[nodiscard]] async::Awaitable<core::Result<automation::TriggeredJobRecord>>
seed_triggered_job(automation::AutomationRuntime& runtime, std::string job_key, std::string trigger_key) {
  co_return co_await runtime.repository().upsert_triggered_job(automation::UpsertTriggeredJobRequest{
      .job_key = std::move(job_key),
      .trigger_key = std::move(trigger_key),
      .agent_key = "automation",
      .agent_prompt = "trigger",
  });
}

[[nodiscard]] async::Awaitable<std::string> read_webhook_response(asio::ip::tcp::socket& socket) {
  asio::error_code ec;
  asio::streambuf response;
  co_await asio::async_read_until(socket, response, "\r\n\r\n", asio::redirect_error(asio::use_awaitable, ec));
  REQUIRE_FALSE(ec);

  std::istream input{&response};
  auto status_line = std::string{};
  REQUIRE(static_cast<bool>(std::getline(input, status_line)));
  auto content_length = std::size_t{0};
  for (auto line = std::string{}; std::getline(input, line);) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      break;
    }
    if (line.starts_with("Content-Length: ")) {
      content_length = static_cast<std::size_t>(std::stoull(line.substr(std::string_view{"Content-Length: "}.size())));
    }
  }

  if (response.size() < content_length) {
    co_await asio::async_read(socket,
                              response,
                              asio::transfer_exactly(content_length - response.size()),
                              asio::redirect_error(asio::use_awaitable, ec));
    REQUIRE_FALSE(ec);
  }

  auto response_body = std::string(content_length, '\0');
  if (content_length > 0) {
    input.read(response_body.data(), static_cast<std::streamsize>(content_length));
  }
  co_return status_line + "\n" + response_body;
}

[[nodiscard]] async::Awaitable<std::string>
post_webhook(asio::io_context& io, std::uint16_t port, std::string path, std::string body) {
  using asio::ip::tcp;
  tcp::socket socket{io};
  asio::error_code ec;
  std::println(stderr, "DBG client connect");
  co_await socket.async_connect(tcp::endpoint{asio::ip::make_address("127.0.0.1"), port},
                                asio::redirect_error(asio::use_awaitable, ec));
  std::println(stderr, "DBG client connected {}", ec.message());
  REQUIRE_FALSE(ec);

  auto request = std::format("POST {} HTTP/1.1\r\n"
                             "Host: 127.0.0.1\r\n"
                             "Content-Type: application/json\r\n"
                             "Content-Length: {}\r\n"
                             "Connection: close\r\n"
                             "\r\n"
                             "{}",
                             path,
                             body.size(),
                             body);
  co_await asio::async_write(socket, asio::buffer(request), asio::redirect_error(asio::use_awaitable, ec));
  std::println(stderr, "DBG client wrote {}", ec.message());
  REQUIRE_FALSE(ec);

  asio::streambuf response;
  co_await asio::async_read_until(socket, response, "\r\n\r\n", asio::redirect_error(asio::use_awaitable, ec));
  std::println(stderr, "DBG client response {}", ec.message());
  REQUIRE_FALSE(ec);

  std::istream input{&response};
  auto status_line = std::string{};
  REQUIRE(static_cast<bool>(std::getline(input, status_line)));
  auto content_length = std::size_t{0};
  for (auto line = std::string{}; std::getline(input, line);) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      break;
    }
    if (line.starts_with("Content-Length: ")) {
      content_length = static_cast<std::size_t>(std::stoull(line.substr(std::string_view{"Content-Length: "}.size())));
    }
  }

  if (response.size() < content_length) {
    co_await asio::async_read(socket,
                              response,
                              asio::transfer_exactly(content_length - response.size()),
                              asio::redirect_error(asio::use_awaitable, ec));
    REQUIRE_FALSE(ec);
  }

  auto response_body = std::string(content_length, '\0');
  if (content_length > 0) {
    input.read(response_body.data(), static_cast<std::streamsize>(content_length));
  }
  co_return status_line + "\n" + response_body;
}

[[nodiscard]] async::Awaitable<void> run_webhook_listener_for_test(asio::io_context& io,
                                                                   automation::AutomationService& service,
                                                                   bootstrap::ServeWebhookOptions options,
                                                                   std::atomic_bool& stop_requested,
                                                                   async::Channel<bool>& done) {
  auto result = co_await bootstrap::serve_webhooks(io.get_executor(), service, std::move(options), [&stop_requested] {
    return stop_requested.load(std::memory_order_acquire);
  });
  [[maybe_unused]] auto sent = done.try_send(result.has_value());
}

}  // namespace

TEST_CASE("serve_automation fires a due cron job, then stops gracefully on cancel",
          "[unit][bootstrap][serve][automation]") {
  TempDir temp{"oran-serve-automation-due"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    const auto db = (temp.path() / ".orangutan" / "automation.db").string();
    auto runtime =
        co_await automation::AutomationRuntime::open(io.get_executor(),
                                                     automation::AutomationRuntimeOptions{.database_path = db});
    REQUIRE(runtime.has_value());
    REQUIRE((co_await seed_cron_job(*runtime, "cron:tick", -2min)).has_value());

    auto service = runtime->automation_service();
    int cron_calls{};
    automation::CronJobHandler cron_handler =
        [&cron_calls](automation::CronDueJob) -> async::Awaitable<core::Result<void>> {
      ++cron_calls;
      co_return core::Result<void>{};
    };
    automation::TriggeredJobHandler triggered_handler =
        [](automation::TriggeredExecutionJob) -> async::Awaitable<core::Result<void>> {
      co_return core::Result<void>{};
    };

    using namespace asio::experimental::awaitable_operators;
    [[maybe_unused]] auto raced =
        co_await (bootstrap::serve_automation(io.get_executor(),
                                              service,
                                              std::move(cron_handler),
                                              std::move(triggered_handler),
                                              bootstrap::ServeAutomationOptions{.poll_interval = 5ms},
                                              [&cron_calls] { return cron_calls >= 1; }) ||
                  async::sleep_for(io.get_executor(), 500ms));

    // The due job fired at least once; the cooperative predicate then stopped
    // the loop (the racing sleep is only a safety net against a hang).
    REQUIRE(cron_calls >= 1);
  });
}

TEST_CASE("serve_automation drains queued triggered work before cron",
          "[unit][bootstrap][serve][automation][triggered]") {
  TempDir temp{"oran-serve-automation-triggered"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    const auto db = (temp.path() / ".orangutan" / "automation.db").string();
    auto runtime =
        co_await automation::AutomationRuntime::open(io.get_executor(),
                                                     automation::AutomationRuntimeOptions{.database_path = db});
    REQUIRE(runtime.has_value());
    REQUIRE((co_await seed_triggered_job(*runtime, "triggered:webhook-ci", "webhook:ci")).has_value());

    auto service = runtime->automation_service();
    auto enqueued = co_await service.enqueue_triggered(automation::TriggeredQueueEnqueueRequest{
        .trigger_key = "webhook:ci",
        .received_at = core::time::now_utc(),
        .job_limit = 10,
    });
    REQUIRE(enqueued.has_value());
    REQUIRE(enqueued->enqueued_count == 1);

    int cron_calls{};
    int triggered_calls{};
    automation::CronJobHandler cron_handler =
        [&cron_calls](automation::CronDueJob) -> async::Awaitable<core::Result<void>> {
      ++cron_calls;
      co_return core::Result<void>{};
    };
    automation::TriggeredJobHandler triggered_handler =
        [&triggered_calls](automation::TriggeredExecutionJob execution) -> async::Awaitable<core::Result<void>> {
      ++triggered_calls;
      REQUIRE(execution.job.job_key == "triggered:webhook-ci");
      REQUIRE(execution.trigger_key == "webhook:ci");
      co_return core::Result<void>{};
    };

    using namespace asio::experimental::awaitable_operators;
    [[maybe_unused]] auto raced =
        co_await (bootstrap::serve_automation(io.get_executor(),
                                              service,
                                              std::move(cron_handler),
                                              std::move(triggered_handler),
                                              bootstrap::ServeAutomationOptions{.poll_interval = 5ms},
                                              [&triggered_calls] { return triggered_calls >= 1; }) ||
                  async::sleep_for(io.get_executor(), 500ms));

    REQUIRE(triggered_calls == 1);
    REQUIRE(cron_calls == 0);
    auto runs = co_await runtime->repository().list_triggered_runs(automation::ListTriggeredRunsOptions{
        .job_key = "triggered:webhook-ci",
        .limit = 10,
    });
    REQUIRE(runs.has_value());
    REQUIRE(runs->size() == 1);
    REQUIRE(runs->front().outcome == automation::TriggeredRunOutcome::success);
  });
}

TEST_CASE("serve_webhooks accepts POST payloads and enqueues triggered work",
          "[unit][bootstrap][serve][automation][webhook]") {
  TempDir temp{"oran-serve-webhook"};
  test::run_async(
      [&temp](asio::io_context& io) -> async::Awaitable<void> {
        const auto db = (temp.path() / ".orangutan" / "automation.db").string();
        auto runtime =
            co_await automation::AutomationRuntime::open(io.get_executor(),
                                                         automation::AutomationRuntimeOptions{.database_path = db});
        REQUIRE(runtime.has_value());
        REQUIRE((co_await seed_triggered_job(*runtime, "triggered:webhook-ci", "webhook:ci")).has_value());

        auto service = runtime->automation_service();
        std::atomic_bool stop_requested{false};
        async::Channel<std::uint16_t> bound_port{io.get_executor(), 1};
        async::Channel<bool> listener_done{io.get_executor(), 1};

        auto options = bootstrap::ServeWebhookOptions{
            .bind_host = "127.0.0.1",
            .port = 0,
            .path_prefix = "/automation/webhooks/",
            .max_payload_bytes = 4096,
            .max_header_bytes = 256,
            .max_connections = 64,
            .header_timeout = 5s,
            .read_timeout = 10s,
            .write_timeout = 5s,
            .job_limit = 10,
            .bound_observer =
                [&bound_port](std::uint16_t port) { [[maybe_unused]] auto sent = bound_port.try_send(port); },
        };
        asio::co_spawn(io,
                       run_webhook_listener_for_test(io, service, std::move(options), stop_requested, listener_done),
                       asio::detached);

        auto port = co_await bound_port.receive();
        REQUIRE(port.has_value());
        const auto payload = std::string(512, 'x');
        auto response = co_await post_webhook(io, *port, "/automation/webhooks/ci", payload);
        CHECK(response.starts_with("HTTP/1.1 202 Accepted"));
        CHECK(response.contains(R"("trigger_key":"webhook:ci")"));
        CHECK(response.contains(R"("enqueued":1)"));

        stop_requested.store(true, std::memory_order_release);
        auto listener_stopped = co_await listener_done.receive();
        REQUIRE(listener_stopped.has_value());
        REQUIRE(*listener_stopped);

        std::optional<std::string> captured_payload;
        automation::CronJobHandler cron_handler = [](automation::CronDueJob) -> async::Awaitable<core::Result<void>> {
          co_return core::Result<void>{};
        };
        automation::TriggeredJobHandler triggered_handler =
            [&captured_payload](automation::TriggeredExecutionJob execution) -> async::Awaitable<core::Result<void>> {
          REQUIRE(execution.job.job_key == "triggered:webhook-ci");
          REQUIRE(execution.trigger_key == "webhook:ci");
          captured_payload = execution.trigger_payload;
          co_return core::Result<void>{};
        };
        auto ran = co_await service.run(automation::AutomationServiceRunRequest{
            .cycle =
                automation::AutomationServiceCycleRequest{
                    .now = core::time::now_utc(),
                    .max_total_wait = std::chrono::steady_clock::duration::zero(),
                    .max_iterations = 1,
                    .cron_job_limit = 10,
                    .cron_handler = std::move(cron_handler),
                    .triggered_handler = std::move(triggered_handler),
                    .triggered_max_jobs = 10,
                },
            .max_iterations = 1,
            .retry_wait = std::chrono::steady_clock::duration::zero(),
        });
        REQUIRE(ran.has_value());
        REQUIRE(captured_payload == std::optional<std::string>{payload});
      },
      10s);
}

TEST_CASE("serve_webhooks bounds concurrent incomplete connections",
          "[unit][bootstrap][serve][automation][webhook][security]") {
  TempDir temp{"oran-serve-webhook-connection-cap"};
  test::run_async(
      [&temp](asio::io_context& io) -> async::Awaitable<void> {
        const auto db = (temp.path() / ".orangutan" / "automation.db").string();
        auto runtime =
            co_await automation::AutomationRuntime::open(io.get_executor(),
                                                         automation::AutomationRuntimeOptions{.database_path = db});
        REQUIRE(runtime.has_value());

        auto service = runtime->automation_service();
        std::atomic_bool stop_requested{false};
        async::Channel<std::uint16_t> bound_port{io.get_executor(), 1};
        async::Channel<bool> listener_done{io.get_executor(), 1};
        auto options = bootstrap::ServeWebhookOptions{
            .bind_host = "127.0.0.1",
            .port = 0,
            .path_prefix = "/automation/webhooks/",
            .max_payload_bytes = 4096,
            .max_connections = 1,
            .header_timeout = 1s,
            .job_limit = 10,
            .bound_observer =
                [&bound_port](std::uint16_t port) { [[maybe_unused]] auto sent = bound_port.try_send(port); },
        };
        asio::co_spawn(io,
                       run_webhook_listener_for_test(io, service, std::move(options), stop_requested, listener_done),
                       asio::detached);

        auto port = co_await bound_port.receive();
        REQUIRE(port.has_value());
        using asio::ip::tcp;
        auto connect_incomplete = [&](tcp::socket& socket) -> async::Awaitable<void> {
          asio::error_code ec;
          socket.connect(tcp::endpoint{asio::ip::make_address("127.0.0.1"), *port}, ec);
          REQUIRE_FALSE(ec);
          const auto partial = std::string{"POST /automation/webhooks/ci HTTP/1.1\r\n"};
          asio::write(socket, asio::buffer(partial), ec);
          REQUIRE_FALSE(ec);
          co_await asio::post(io, asio::use_awaitable);
          co_await asio::post(io, asio::use_awaitable);
        };

        tcp::socket first{io};
        co_await connect_incomplete(first);

        auto overloaded = co_await post_webhook(io, *port, "/automation/webhooks/ci", "{}");
        CHECK(overloaded.starts_with("HTTP/1.1 503 Service Unavailable"));

        asio::error_code ignored;
        first.close(ignored);
        stop_requested.store(true, std::memory_order_release);
        auto listener_stopped = co_await listener_done.receive();
        REQUIRE(listener_stopped.has_value());
        REQUIRE(*listener_stopped);
      },
      3s);
}

TEST_CASE("serve_webhooks caps and times out incomplete request headers",
          "[unit][bootstrap][serve][automation][webhook][security]") {
  TempDir temp{"oran-serve-webhook-header-timeout"};
  test::run_async(
      [&temp](asio::io_context& io) -> async::Awaitable<void> {
        const auto db = (temp.path() / ".orangutan" / "automation.db").string();
        auto runtime =
            co_await automation::AutomationRuntime::open(io.get_executor(),
                                                         automation::AutomationRuntimeOptions{.database_path = db});
        REQUIRE(runtime.has_value());

        auto service = runtime->automation_service();
        std::atomic_bool stop_requested{false};
        async::Channel<std::uint16_t> bound_port{io.get_executor(), 1};
        async::Channel<bool> listener_done{io.get_executor(), 1};
        auto options = bootstrap::ServeWebhookOptions{
            .bind_host = "127.0.0.1",
            .port = 0,
            .path_prefix = "/automation/webhooks/",
            .max_payload_bytes = 4096,
            .max_header_bytes = 128,
            .header_timeout = 30ms,
            .job_limit = 10,
            .bound_observer =
                [&bound_port](std::uint16_t port) { [[maybe_unused]] auto sent = bound_port.try_send(port); },
        };
        asio::co_spawn(io,
                       run_webhook_listener_for_test(io, service, std::move(options), stop_requested, listener_done),
                       asio::detached);

        auto port = co_await bound_port.receive();
        REQUIRE(port.has_value());
        using asio::ip::tcp;
        tcp::socket oversized{io};
        asio::error_code ec;
        co_await oversized.async_connect(tcp::endpoint{asio::ip::make_address("127.0.0.1"), *port},
                                         asio::redirect_error(asio::use_awaitable, ec));
        REQUIRE_FALSE(ec);
        const auto oversized_headers =
            std::format("POST /automation/webhooks/ci HTTP/1.1\r\nX-Oversized: {}\r\n\r\n", std::string(200, 'x'));
        co_await asio::async_write(oversized,
                                   asio::buffer(oversized_headers),
                                   asio::redirect_error(asio::use_awaitable, ec));
        REQUIRE_FALSE(ec);
        auto rejected = co_await read_webhook_response(oversized);
        CHECK(rejected.starts_with("HTTP/1.1 431 Request Header Fields Too Large"));

        tcp::socket socket{io};
        co_await socket.async_connect(tcp::endpoint{asio::ip::make_address("127.0.0.1"), *port},
                                      asio::redirect_error(asio::use_awaitable, ec));
        REQUIRE_FALSE(ec);
        const auto partial = std::string{"POST /automation/webhooks/ci HTTP/1.1\r\nHost: 127.0.0.1\r\n"};
        co_await asio::async_write(socket, asio::buffer(partial), asio::redirect_error(asio::use_awaitable, ec));
        REQUIRE_FALSE(ec);

        auto timed_out = co_await read_webhook_response(socket);
        CHECK(timed_out.starts_with("HTTP/1.1 408 Request Timeout"));

        stop_requested.store(true, std::memory_order_release);
        auto listener_stopped = co_await listener_done.receive();
        REQUIRE(listener_stopped.has_value());
        REQUIRE(*listener_stopped);
      },
      3s);
}

TEST_CASE("serve_webhooks refuses unauthenticated non-loopback binds",
          "[unit][bootstrap][serve][automation][webhook][security]") {
  TempDir temp{"oran-serve-webhook-bind"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    const auto db = (temp.path() / ".orangutan" / "automation.db").string();
    auto runtime =
        co_await automation::AutomationRuntime::open(io.get_executor(),
                                                     automation::AutomationRuntimeOptions{.database_path = db});
    REQUIRE(runtime.has_value());
    auto service = runtime->automation_service();

    for (const auto host : {"0.0.0.0", "::"}) {
      auto result = co_await bootstrap::serve_webhooks(io.get_executor(),
                                                       service,
                                                       bootstrap::ServeWebhookOptions{.bind_host = host, .port = 0});
      REQUIRE_FALSE(result.has_value());
      CHECK(result.error().kind() == core::ErrorKind::config);
      CHECK(result.error().message().contains("must be loopback"));
    }
  });
}

TEST_CASE("serve_webhooks drains open connections on cooperative stop",
          "[unit][bootstrap][serve][automation][webhook]") {
  TempDir temp{"oran-serve-webhook-drain"};
  test::run_async(
      [&temp](asio::io_context& io) -> async::Awaitable<void> {
        const auto db = (temp.path() / ".orangutan" / "automation.db").string();
        auto runtime =
            co_await automation::AutomationRuntime::open(io.get_executor(),
                                                         automation::AutomationRuntimeOptions{.database_path = db});
        REQUIRE(runtime.has_value());

        auto service = runtime->automation_service();
        std::atomic_bool stop_requested{false};
        async::Channel<std::uint16_t> bound_port{io.get_executor(), 1};
        async::Channel<bool> listener_done{io.get_executor(), 1};

        auto options = bootstrap::ServeWebhookOptions{
            .bind_host = "127.0.0.1",
            .port = 0,
            .path_prefix = "/automation/webhooks/",
            .max_payload_bytes = 4096,
            .job_limit = 10,
            .bound_observer =
                [&bound_port](std::uint16_t port) { [[maybe_unused]] auto sent = bound_port.try_send(port); },
        };
        asio::co_spawn(io,
                       run_webhook_listener_for_test(io, service, std::move(options), stop_requested, listener_done),
                       asio::detached);

        auto port = co_await bound_port.receive();
        REQUIRE(port.has_value());

        using asio::ip::tcp;
        tcp::socket socket{io};
        asio::error_code ec;
        co_await socket.async_connect(tcp::endpoint{asio::ip::make_address("127.0.0.1"), *port},
                                      asio::redirect_error(asio::use_awaitable, ec));
        REQUIRE_FALSE(ec);

        auto partial_request = std::string{"POST /automation/webhooks/ci HTTP/1.1\r\n"
                                           "Host: 127.0.0.1\r\n"
                                           "Content-Length: 4\r\n"};
        co_await asio::async_write(socket,
                                   asio::buffer(partial_request),
                                   asio::redirect_error(asio::use_awaitable, ec));
        REQUIRE_FALSE(ec);

        [[maybe_unused]] auto accepted = co_await async::sleep_for(io.get_executor(), 20ms);
        stop_requested.store(true, std::memory_order_release);

        using namespace asio::experimental::awaitable_operators;
        auto done = co_await (listener_done.receive() || async::sleep_for(io.get_executor(), 1s));
        REQUIRE(done.index() == 0);
        auto stopped = std::get<0>(std::move(done));
        REQUIRE(stopped.has_value());
        REQUIRE(*stopped);

        socket.close(ec);
      },
      3s);
}

TEST_CASE("serve_automation idles when no cron job is due and stops on cancel",
          "[unit][bootstrap][serve][automation]") {
  TempDir temp{"oran-serve-automation-idle"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    const auto db = (temp.path() / ".orangutan" / "automation.db").string();
    auto runtime =
        co_await automation::AutomationRuntime::open(io.get_executor(),
                                                     automation::AutomationRuntimeOptions{.database_path = db});
    REQUIRE(runtime.has_value());
    REQUIRE((co_await seed_cron_job(*runtime, "cron:future", 1h)).has_value());

    auto service = runtime->automation_service();
    int cron_calls{};
    automation::CronJobHandler cron_handler =
        [&cron_calls](automation::CronDueJob) -> async::Awaitable<core::Result<void>> {
      ++cron_calls;
      co_return core::Result<void>{};
    };
    automation::TriggeredJobHandler triggered_handler =
        [](automation::TriggeredExecutionJob) -> async::Awaitable<core::Result<void>> {
      co_return core::Result<void>{};
    };

    using namespace asio::experimental::awaitable_operators;
    [[maybe_unused]] auto raced =
        co_await (bootstrap::serve_automation(io.get_executor(),
                                              service,
                                              std::move(cron_handler),
                                              std::move(triggered_handler),
                                              bootstrap::ServeAutomationOptions{.poll_interval = 5ms}) ||
                  async::sleep_for(io.get_executor(), 80ms));

    REQUIRE(cron_calls == 0);
  });
}

TEST_CASE("serve_automation honors an immediate stop predicate without firing",
          "[unit][bootstrap][serve][automation]") {
  TempDir temp{"oran-serve-automation-stop"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    const auto db = (temp.path() / ".orangutan" / "automation.db").string();
    auto runtime =
        co_await automation::AutomationRuntime::open(io.get_executor(),
                                                     automation::AutomationRuntimeOptions{.database_path = db});
    REQUIRE(runtime.has_value());
    REQUIRE((co_await seed_cron_job(*runtime, "cron:tick", -2min)).has_value());

    auto service = runtime->automation_service();
    int cron_calls{};
    automation::CronJobHandler cron_handler =
        [&cron_calls](automation::CronDueJob) -> async::Awaitable<core::Result<void>> {
      ++cron_calls;
      co_return core::Result<void>{};
    };
    automation::TriggeredJobHandler triggered_handler =
        [](automation::TriggeredExecutionJob) -> async::Awaitable<core::Result<void>> {
      co_return core::Result<void>{};
    };

    // An already-true predicate must return at the loop's first guard, before any
    // cycle runs, so the otherwise-due job never fires and no cancellation is
    // needed to stop the loop.
    auto outcome = co_await bootstrap::serve_automation(io.get_executor(),
                                                        service,
                                                        std::move(cron_handler),
                                                        std::move(triggered_handler),
                                                        bootstrap::ServeAutomationOptions{.poll_interval = 5ms},
                                                        [] { return true; });

    REQUIRE(outcome.has_value());
    REQUIRE(cron_calls == 0);
  });
}

namespace {

/// A tool that takes an exclusive per-path lock (via `write_file`) and returns
/// immediately. Running it through the scheduler leaves one idle lock-table
/// entry behind — the input the reaping concern is built to bound.
[[nodiscard]] core::ToolDef lock_write_tool_def(std::string name) {
  return core::ToolDef{
      .name = std::move(name),
      .description = "exclusive-lock test tool",
      .input_schema_json = R"({"type":"object","properties":{"path":{"type":"string"}},"additionalProperties":true})",
      .required_capabilities = {core::Capability::write_file},
      .deferred = false,
      .category = "test",
  };
}

void add_lock_write_tool(tool::Registry& registry, std::string name) {
  auto handler = [](std::string_view, tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
    co_return tool::Output::text_only("written");
  };
  REQUIRE(registry.add(lock_write_tool_def(std::move(name)), std::move(handler)).has_value());
}

[[nodiscard]] tool::Workspace make_workspace(const std::filesystem::path& root) {
  auto workspace = tool::Workspace::create(root.string());
  REQUIRE(workspace.has_value());
  return std::move(*workspace);
}

[[nodiscard]] permission::RuleSet allow_all_rules() {
  permission::RuleSet rules;
  rules.add(permission::Rule{.verdict = permission::Verdict::allow, .tool_pattern = "*", .capability = std::nullopt});
  return rules;
}

[[nodiscard]] tool::DispatchContext make_prototype(asio::io_context& io,
                                                   permission::RuleSet& rules,
                                                   permission::AuditSink& audit,
                                                   tool::Workspace& workspace) {
  return tool::DispatchContext{
      .executor = io.get_executor(),
      .mode = permission::Mode::default_,
      .rules = rules,
      .audit = audit,
      .workspace = &workspace,
      .scope_key = "serve",
      .agent_key = "automation",
      .identity = "automation",
  };
}

[[nodiscard]] agent::ToolBatchCall lock_call(std::string name, std::string_view rel_path) {
  return agent::ToolBatchCall{
      .tool_use_id = "call-0",
      .name = std::move(name),
      .input_json = std::string{R"({"path":")"} + std::string{rel_path} + R"("})",
  };
}

/// Acquire one idle lock-table entry by running a single write-lock tool call
/// through `scheduler`, so the reaping cases start from a populated table.
[[nodiscard]] async::Awaitable<void> populate_one_lock(agent::ToolScheduler& scheduler,
                                                       tool::DispatchContext& prototype) {
  std::vector<agent::ToolBatchCall> batch;
  batch.push_back(lock_call("FakeLockWrite", "d.txt"));
  auto result = co_await scheduler.run_batch(std::move(batch), prototype);
  REQUIRE(result.has_value());
  REQUIRE(scheduler.lock_stats().current_entries == 1);
}

}  // namespace

TEST_CASE("serve_scheduler_reaping reaps idle locks across ticks, then stops on predicate",
          "[unit][bootstrap][serve][scheduler]") {
  TempDir temp{"oran-serve-reap-due"};
  std::ofstream{temp.path() / "d.txt"} << "x";
  test::run_async(
      [&temp](asio::io_context& io) -> async::Awaitable<void> {
        tool::Registry registry;
        add_lock_write_tool(registry, "FakeLockWrite");
        auto workspace = make_workspace(temp.path());
        auto rules = allow_all_rules();
        permission::NullAuditSink audit;
        auto prototype = make_prototype(io, rules, audit, workspace);

        // A 1 ms idle TTL makes the lone entry reapable a tick later.
        agent::ToolScheduler scheduler{
            io.get_executor(),
            registry,
            agent::ToolSchedulerOptions{.max_parallel_tools = 4, .per_call_timeout = 5s, .idle_lock_ttl = 1ms}};
        co_await populate_one_lock(scheduler, prototype);

        using namespace asio::experimental::awaitable_operators;
        [[maybe_unused]] auto raced = co_await (
            bootstrap::serve_scheduler_reaping(io.get_executor(),
                                               scheduler,
                                               bootstrap::ServeSchedulerReapOptions{.reap_interval = 10ms},
                                               [&scheduler] { return scheduler.lock_stats().reaped_entries >= 1; }) ||
            async::sleep_for(io.get_executor(), 1s));

        // A tick fired the reap (entry idle past its 1 ms TTL), then the
        // cooperative predicate stopped the loop.
        REQUIRE(scheduler.lock_stats().reaped_entries >= 1);
        REQUIRE(scheduler.lock_stats().current_entries == 0);
      },
      3s);
}

TEST_CASE("serve_scheduler_reaping honors an immediate stop predicate without reaping",
          "[unit][bootstrap][serve][scheduler]") {
  TempDir temp{"oran-serve-reap-stop"};
  std::ofstream{temp.path() / "d.txt"} << "x";
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    add_lock_write_tool(registry, "FakeLockWrite");
    auto workspace = make_workspace(temp.path());
    auto rules = allow_all_rules();
    permission::NullAuditSink audit;
    auto prototype = make_prototype(io, rules, audit, workspace);

    agent::ToolScheduler scheduler{
        io.get_executor(),
        registry,
        agent::ToolSchedulerOptions{.max_parallel_tools = 4, .per_call_timeout = 5s, .idle_lock_ttl = 1ms}};
    co_await populate_one_lock(scheduler, prototype);

    // An already-true predicate must return at the loop's first guard, before
    // any tick runs — so the otherwise-reapable entry survives untouched.
    auto outcome =
        co_await bootstrap::serve_scheduler_reaping(io.get_executor(),
                                                    scheduler,
                                                    bootstrap::ServeSchedulerReapOptions{.reap_interval = 5ms},
                                                    [] { return true; });

    REQUIRE(outcome.has_value());
    REQUIRE(scheduler.lock_stats().reaped_entries == 0);
    REQUIRE(scheduler.lock_stats().current_entries == 1);
  });
}

TEST_CASE("serve_scheduler_reaping stops gracefully on cancel", "[unit][bootstrap][serve][scheduler]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    agent::ToolScheduler scheduler{io.get_executor(), registry};

    // A long reap interval keeps the concern parked in its first idle wait; the
    // racing sleep fires the cancellation, which must end the wait gracefully.
    using namespace asio::experimental::awaitable_operators;
    [[maybe_unused]] auto raced =
        co_await (bootstrap::serve_scheduler_reaping(io.get_executor(),
                                                     scheduler,
                                                     bootstrap::ServeSchedulerReapOptions{.reap_interval = 1s}) ||
                  async::sleep_for(io.get_executor(), 20ms));

    // Cancelled while still in the first interval, so no tick ever reaped.
    REQUIRE(scheduler.lock_stats().reaped_entries == 0);
  });
}

namespace {

[[nodiscard]] channel::InboundMessage text_inbound(std::string text, std::string conversation_id = "conv-1") {
  return channel::InboundMessage{
      .channel_id = "mock-main",
      .conversation_id = std::move(conversation_id),
      .user_id = "user-1",
      .display_name = "User One",
      .content = {core::TextContent{.text = std::move(text)}},
      .replies_to = {channel::Reference{.message_id = "msg-0", .thread_id = "thread-1"}},
      .received_at = core::Time::epoch(),
      .origin = channel::Origin{.kind = "channel", .source = "mock"},
      .caps = {},
  };
}

}  // namespace

TEST_CASE("ServeChannelMetricsLogSink emits deduplicated channel worker metrics",
          "[unit][bootstrap][serve][channels][metrics]") {
  std::vector<std::string> lines;
  auto sink = bootstrap::ServeChannelMetricsLogSink{bootstrap::ServeChannelMetricsLogSinkOptions{
      .emit_line = [&](std::string line) { lines.push_back(std::move(line)); }}};

  auto snapshot = bootstrap::ServeChannelWorkerMetrics{
      .active_workers = 1,
      .max_active_workers = 2,
      .workers_created = 3,
      .workers_completed = 4,
      .workers_evicted_idle = 5,
      .messages_enqueued = 6,
      .replies_sent = 7,
      .message_timeouts = 8,
      .dispatch_failures = 9,
      .enqueue_failures = 10,
      .conversation_overloads = 11,
  };
  sink(snapshot);
  sink(snapshot);
  ++snapshot.replies_sent;
  sink(snapshot);

  REQUIRE(lines.size() == 2);
  CHECK(lines.front() == bootstrap::format_serve_channel_worker_metrics(bootstrap::ServeChannelWorkerMetrics{
                             .active_workers = 1,
                             .max_active_workers = 2,
                             .workers_created = 3,
                             .workers_completed = 4,
                             .workers_evicted_idle = 5,
                             .messages_enqueued = 6,
                             .replies_sent = 7,
                             .message_timeouts = 8,
                             .dispatch_failures = 9,
                             .enqueue_failures = 10,
                             .conversation_overloads = 11,
                         }));
  CHECK(lines.back().contains("replies=8"));
  CHECK(lines.back().contains("timeouts=8"));
  CHECK(lines.back().contains("overloads=11"));
}

TEST_CASE("serve_channels dispatches mock inbound messages and replies", "[unit][bootstrap][serve][channels]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    channel::ChannelManager manager{io.get_executor()};
    auto adapter =
        std::make_unique<channel::MockChannel>(io.get_executor(),
                                               channel::MockChannelOptions{.id = "mock-main", .kind = "mock"});
    auto* mock = adapter.get();
    REQUIRE(manager.register_adapter(std::move(adapter)).has_value());
    REQUIRE((co_await manager.start_all()).has_value());
    REQUIRE(mock->started());
    REQUIRE(mock->push_inbound(text_inbound("hello from channel")).has_value());

    std::vector<channel::ChannelPromptRunRequest> seen;
    auto runner = channel::ChannelPromptRunner{[&seen](channel::ChannelPromptRunRequest request)
                                                   -> async::Awaitable<core::Result<channel::ChannelPromptRunResult>> {
      seen.push_back(std::move(request));
      co_return channel::ChannelPromptRunResult{.text = "reply from agent"};
    }};

    auto outcome =
        co_await bootstrap::serve_channels(io.get_executor(),
                                           manager,
                                           std::move(runner),
                                           std::vector<std::string>{"mock-main"},
                                           [&seen, mock] { return !seen.empty() && !mock->sent_messages().empty(); });

    REQUIRE(outcome.has_value());
    REQUIRE(seen.size() == 1);
    CHECK(seen.front().channel_id == "mock-main");
    CHECK(seen.front().conversation_id == "conv-1");
    CHECK(seen.front().user_id == "user-1");
    CHECK(seen.front().display_name == "User One");
    CHECK(seen.front().prompt == "hello from channel");

    REQUIRE(mock->sent_messages().size() == 1);
    const auto& sent = mock->sent_messages().front();
    REQUIRE(sent.content.size() == 1);
    auto sent_text = core::text_view(sent.content.front());
    REQUIRE(sent_text.has_value());
    CHECK(*sent_text == "reply from agent");
    CHECK(sent.conversation_id == "conv-1");
    REQUIRE(sent.reply_to_message_id.has_value());
    CHECK(*sent.reply_to_message_id == "msg-0");
    REQUIRE(sent.thread_id.has_value());
    CHECK(*sent.thread_id == "thread-1");
    CHECK_FALSE(mock->started());
  });
}

TEST_CASE("serve_channels serializes each conversation without blocking others",
          "[unit][bootstrap][serve][channels][ordering]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    channel::ChannelManager manager{io.get_executor()};
    auto adapter =
        std::make_unique<channel::MockChannel>(io.get_executor(),
                                               channel::MockChannelOptions{.id = "mock-main", .kind = "mock"});
    auto* mock = adapter.get();
    REQUIRE(manager.register_adapter(std::move(adapter)).has_value());
    REQUIRE((co_await manager.start_all()).has_value());
    REQUIRE(mock->push_inbound(text_inbound("first", "conv-1")).has_value());
    REQUIRE(mock->push_inbound(text_inbound("second", "conv-1")).has_value());
    REQUIRE(mock->push_inbound(text_inbound("other", "conv-2")).has_value());

    std::vector<std::string> started;
    auto runner =
        channel::ChannelPromptRunner{[&started, executor = io.get_executor()](channel::ChannelPromptRunRequest request)
                                         -> async::Awaitable<core::Result<channel::ChannelPromptRunResult>> {
          started.push_back(request.conversation_id + ":" + request.prompt);
          if (request.conversation_id == "conv-1" && request.prompt == "first") {
            auto slept = co_await async::sleep_for(executor, 25ms);
            if (!slept) {
              co_return std::unexpected(std::move(slept).error());
            }
          }
          co_return channel::ChannelPromptRunResult{.text = request.prompt + " reply"};
        }};

    auto outcome = co_await bootstrap::serve_channels(io.get_executor(),
                                                      manager,
                                                      std::move(runner),
                                                      std::vector<std::string>{"mock-main"},
                                                      [mock] { return mock->sent_messages().size() >= 3; });

    REQUIRE(outcome.has_value());
    REQUIRE(mock->sent_messages().size() == 3);
    auto first = std::ranges::find(started, std::string{"conv-1:first"});
    auto second = std::ranges::find(started, std::string{"conv-1:second"});
    auto other = std::ranges::find(started, std::string{"conv-2:other"});
    REQUIRE(first != started.end());
    REQUIRE(second != started.end());
    REQUIRE(other != started.end());
    CHECK(first < second);
    CHECK(other < second);
    CHECK_FALSE(mock->started());
  });
}

TEST_CASE("serve_channels evicts idle conversation workers before cooperative stop",
          "[unit][bootstrap][serve][channels][idle]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    channel::ChannelManager manager{io.get_executor()};
    auto adapter =
        std::make_unique<channel::MockChannel>(io.get_executor(),
                                               channel::MockChannelOptions{.id = "mock-main", .kind = "mock"});
    auto* mock = adapter.get();
    REQUIRE(manager.register_adapter(std::move(adapter)).has_value());
    REQUIRE((co_await manager.start_all()).has_value());
    REQUIRE(mock->push_inbound(text_inbound("first", "conv-1")).has_value());

    std::atomic_bool saw_post_reply_check{false};
    std::atomic_bool request_stop{false};
    std::atomic_bool driver_done{false};

    asio::co_spawn(
        io,
        [&]() -> async::Awaitable<void> {
          while (mock->sent_messages().empty()) {
            [[maybe_unused]] auto slept = co_await async::sleep_for(io.get_executor(), 1ms);
          }
          while (!saw_post_reply_check.load(std::memory_order_acquire)) {
            [[maybe_unused]] auto slept = co_await async::sleep_for(io.get_executor(), 1ms);
          }
          request_stop.store(true, std::memory_order_release);
          driver_done.store(true, std::memory_order_release);
        },
        asio::detached);

    auto runner = channel::ChannelPromptRunner{[](channel::ChannelPromptRunRequest request)
                                                   -> async::Awaitable<core::Result<channel::ChannelPromptRunResult>> {
      co_return channel::ChannelPromptRunResult{.text = request.prompt + " reply"};
    }};

    auto outcome = co_await bootstrap::serve_channels(
        io.get_executor(),
        manager,
        std::move(runner),
        std::vector<std::string>{"mock-main"},
        [&] {
          if (mock->sent_messages().size() == 1) {
            saw_post_reply_check.store(true, std::memory_order_release);
          }
          return request_stop.load(std::memory_order_acquire);
        },
        nullptr,
        bootstrap::ServeChannelOptions{.conversation_idle_ttl = 50ms});

    REQUIRE(outcome.has_value());
    REQUIRE(mock->sent_messages().size() == 1);
    CHECK(driver_done.load(std::memory_order_acquire));
    CHECK_FALSE(mock->started());
  });
}

TEST_CASE("serve_channels accepts an enqueue at the idle-retirement boundary",
          "[unit][bootstrap][serve][channels][idle]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    channel::ChannelManager manager{io.get_executor()};
    auto adapter =
        std::make_unique<channel::MockChannel>(io.get_executor(),
                                               channel::MockChannelOptions{.id = "mock-main", .kind = "mock"});
    auto* mock = adapter.get();
    REQUIRE(manager.register_adapter(std::move(adapter)).has_value());
    REQUIRE((co_await manager.start_all()).has_value());
    REQUIRE(mock->push_inbound(text_inbound("first", "conv-1")).has_value());

    bool injected_at_retirement = false;
    bool injection_succeeded = false;
    std::vector<bootstrap::ServeChannelWorkerMetrics> snapshots;
    auto runner = channel::ChannelPromptRunner{[](channel::ChannelPromptRunRequest request)
                                                   -> async::Awaitable<core::Result<channel::ChannelPromptRunResult>> {
      co_return channel::ChannelPromptRunResult{.text = request.prompt + " reply"};
    }};

    auto outcome = co_await bootstrap::serve_channels(
        io.get_executor(),
        manager,
        std::move(runner),
        std::vector<std::string>{"mock-main"},
        [mock] { return mock->sent_messages().size() == 2; },
        nullptr,
        bootstrap::ServeChannelOptions{
            .max_active_conversations = 1,
            .conversation_idle_ttl = 10ms,
            .metrics_observer =
                [&](const bootstrap::ServeChannelWorkerMetrics& snapshot) {
                  snapshots.push_back(snapshot);
                  if (!injected_at_retirement && snapshot.workers_evicted_idle == 1) {
                    injected_at_retirement = true;
                    injection_succeeded = manager.inbound().try_send(text_inbound("second", "conv-1")).has_value();
                  }
                },
        });

    REQUIRE(outcome.has_value());
    CHECK(injected_at_retirement);
    CHECK(injection_succeeded);
    REQUIRE(mock->sent_messages().size() == 2);
    CHECK(snapshots.back().messages_enqueued == 2);
    CHECK(snapshots.back().conversation_overloads == 0);
    CHECK_FALSE(mock->started());
  });
}

TEST_CASE("serve_channels rejects only new conversations at the active-worker cap",
          "[unit][bootstrap][serve][channels][capacity]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    channel::ChannelManager manager{io.get_executor()};
    auto adapter =
        std::make_unique<channel::MockChannel>(io.get_executor(),
                                               channel::MockChannelOptions{.id = "mock-main", .kind = "mock"});
    auto* mock = adapter.get();
    REQUIRE(manager.register_adapter(std::move(adapter)).has_value());
    REQUIRE((co_await manager.start_all()).has_value());
    REQUIRE(mock->push_inbound(text_inbound("first", "conv-1")).has_value());
    REQUIRE(mock->push_inbound(text_inbound("overload", "conv-2")).has_value());
    REQUIRE(mock->push_inbound(text_inbound("second", "conv-1")).has_value());

    std::vector<bootstrap::ServeChannelWorkerMetrics> snapshots;
    auto runner = channel::ChannelPromptRunner{[executor = io.get_executor()](channel::ChannelPromptRunRequest request)
                                                   -> async::Awaitable<core::Result<channel::ChannelPromptRunResult>> {
      if (request.prompt == "first") {
        auto slept = co_await async::sleep_for(executor, 25ms);
        if (!slept) {
          co_return std::unexpected(std::move(slept).error());
        }
      }
      co_return channel::ChannelPromptRunResult{.text = request.prompt + " reply"};
    }};

    auto outcome = co_await bootstrap::serve_channels(
        io.get_executor(),
        manager,
        std::move(runner),
        std::vector<std::string>{"mock-main"},
        [&] {
          return mock->sent_messages().size() == 2 && !snapshots.empty() &&
                 snapshots.back().conversation_overloads == 1;
        },
        nullptr,
        bootstrap::ServeChannelOptions{
            .max_active_conversations = 1,
            .metrics_observer =
                [&](const bootstrap::ServeChannelWorkerMetrics& snapshot) { snapshots.push_back(snapshot); },
        });

    REQUIRE(outcome.has_value());
    REQUIRE(mock->sent_messages().size() == 2);
    CHECK(mock->sent_messages()[0].conversation_id == "conv-1");
    CHECK(mock->sent_messages()[1].conversation_id == "conv-1");
    REQUIRE_FALSE(snapshots.empty());
    const auto final = snapshots.back();
    CHECK(final.max_active_workers == 1);
    CHECK(final.messages_enqueued == 2);
    CHECK(final.enqueue_failures == 1);
    CHECK(final.conversation_overloads == 1);
    CHECK_FALSE(mock->started());
  });
}

TEST_CASE("serve_channels sends still-working reply when message deadline expires",
          "[unit][bootstrap][serve][channels][deadline]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    channel::ChannelManager manager{io.get_executor()};
    auto adapter =
        std::make_unique<channel::MockChannel>(io.get_executor(),
                                               channel::MockChannelOptions{.id = "mock-main", .kind = "mock"});
    auto* mock = adapter.get();
    REQUIRE(manager.register_adapter(std::move(adapter)).has_value());
    REQUIRE((co_await manager.start_all()).has_value());
    REQUIRE(mock->push_inbound(text_inbound("slow request", "conv-1")).has_value());

    std::atomic_bool runner_cancelled{false};
    std::vector<bootstrap::ServeChannelWorkerMetrics> snapshots;
    auto runner = channel::ChannelPromptRunner{
        [executor = io.get_executor(), &runner_cancelled](channel::ChannelPromptRunRequest request)
            -> async::Awaitable<core::Result<channel::ChannelPromptRunResult>> {
          auto slept = co_await async::sleep_for(executor, 250ms);
          if (!slept) {
            if (slept.error().kind() == core::ErrorKind::cancelled) {
              runner_cancelled.store(true, std::memory_order_release);
            }
            co_return std::unexpected(std::move(slept).error());
          }
          co_return channel::ChannelPromptRunResult{.text = request.prompt + " late reply"};
        }};

    auto outcome = co_await bootstrap::serve_channels(
        io.get_executor(),
        manager,
        std::move(runner),
        std::vector<std::string>{"mock-main"},
        [mock] { return mock->sent_messages().size() == 1; },
        nullptr,
        bootstrap::ServeChannelOptions{
            .message_deadline = 5ms,
            .metrics_observer =
                [&](const bootstrap::ServeChannelWorkerMetrics& snapshot) { snapshots.push_back(snapshot); },
        });

    REQUIRE(outcome.has_value());
    REQUIRE(mock->sent_messages().size() == 1);
    const auto& sent = mock->sent_messages().front();
    REQUIRE(sent.content.size() == 1);
    auto sent_text = core::text_view(sent.content.front());
    REQUIRE(sent_text.has_value());
    CHECK(*sent_text == "Still working on that. This request is taking longer than expected.");
    CHECK(sent.conversation_id == "conv-1");
    CHECK(runner_cancelled.load(std::memory_order_acquire));
    REQUIRE_FALSE(snapshots.empty());
    const auto final = snapshots.back();
    CHECK(final.message_timeouts == 1);
    CHECK(final.replies_sent == 1);
    CHECK(final.dispatch_failures == 0);
    CHECK_FALSE(mock->started());
  });
}

TEST_CASE("serve_channels reports conversation worker metrics", "[unit][bootstrap][serve][channels][metrics]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    channel::ChannelManager manager{io.get_executor()};
    auto adapter =
        std::make_unique<channel::MockChannel>(io.get_executor(),
                                               channel::MockChannelOptions{.id = "mock-main", .kind = "mock"});
    auto* mock = adapter.get();
    REQUIRE(manager.register_adapter(std::move(adapter)).has_value());
    REQUIRE((co_await manager.start_all()).has_value());
    REQUIRE(mock->push_inbound(text_inbound("first", "conv-1")).has_value());
    REQUIRE(mock->push_inbound(text_inbound("second", "conv-2")).has_value());

    std::vector<bootstrap::ServeChannelWorkerMetrics> snapshots;
    std::vector<std::string> metric_lines;
    auto sink = bootstrap::ServeChannelMetricsLogSink{bootstrap::ServeChannelMetricsLogSinkOptions{
        .emit_line = [&](std::string line) { metric_lines.push_back(std::move(line)); }}};
    auto runner = channel::ChannelPromptRunner{[](channel::ChannelPromptRunRequest request)
                                                   -> async::Awaitable<core::Result<channel::ChannelPromptRunResult>> {
      co_return channel::ChannelPromptRunResult{.text = request.prompt + " reply"};
    }};

    auto outcome = co_await bootstrap::serve_channels(
        io.get_executor(),
        manager,
        std::move(runner),
        std::vector<std::string>{"mock-main"},
        [&] { return !snapshots.empty() && snapshots.back().workers_evicted_idle >= 2; },
        nullptr,
        bootstrap::ServeChannelOptions{
            .conversation_idle_ttl = 25ms,
            .metrics_observer =
                [&](const bootstrap::ServeChannelWorkerMetrics& snapshot) mutable {
                  snapshots.push_back(snapshot);
                  sink(snapshot);
                },
        });

    REQUIRE(outcome.has_value());
    REQUIRE(mock->sent_messages().size() == 2);
    REQUIRE_FALSE(snapshots.empty());
    const auto final = snapshots.back();
    CHECK(final.active_workers == 0);
    CHECK(final.max_active_workers == 2);
    CHECK(final.workers_created == 2);
    CHECK(final.workers_completed == 2);
    CHECK(final.workers_evicted_idle == 2);
    CHECK(final.messages_enqueued == 2);
    CHECK(final.replies_sent == 2);
    CHECK(final.message_timeouts == 0);
    CHECK(final.dispatch_failures == 0);
    CHECK(final.enqueue_failures == 0);
    REQUIRE_FALSE(metric_lines.empty());
    CHECK(metric_lines.back() == bootstrap::format_serve_channel_worker_metrics(final));
    CHECK_FALSE(mock->started());
  });
}

TEST_CASE("serve_channels keeps serving when worker metrics observer throws",
          "[unit][bootstrap][serve][channels][metrics]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    channel::ChannelManager manager{io.get_executor()};
    auto adapter =
        std::make_unique<channel::MockChannel>(io.get_executor(),
                                               channel::MockChannelOptions{.id = "mock-main", .kind = "mock"});
    auto* mock = adapter.get();
    REQUIRE(manager.register_adapter(std::move(adapter)).has_value());
    REQUIRE((co_await manager.start_all()).has_value());
    REQUIRE(mock->push_inbound(text_inbound("hello", "conv-1")).has_value());

    std::size_t observer_calls{};
    auto runner = channel::ChannelPromptRunner{[](channel::ChannelPromptRunRequest request)
                                                   -> async::Awaitable<core::Result<channel::ChannelPromptRunResult>> {
      co_return channel::ChannelPromptRunResult{.text = request.prompt + " reply"};
    }};

    auto outcome = co_await bootstrap::serve_channels(
        io.get_executor(),
        manager,
        std::move(runner),
        std::vector<std::string>{"mock-main"},
        [mock] { return mock->sent_messages().size() == 1; },
        nullptr,
        bootstrap::ServeChannelOptions{
            .metrics_observer =
                [&](const bootstrap::ServeChannelWorkerMetrics&) {
                  ++observer_calls;
                  throw std::runtime_error{"metrics unavailable"};
                },
        });

    REQUIRE(outcome.has_value());
    REQUIRE(mock->sent_messages().size() == 1);
    CHECK(observer_calls > 0);
    CHECK_FALSE(mock->started());
  });
}

TEST_CASE("serve_channels enqueues matching automation triggers before replying",
          "[unit][bootstrap][serve][channels][automation][triggered]") {
  TempDir temp{"oran-serve-channel-triggered"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    const auto db = (temp.path() / ".orangutan" / "automation.db").string();
    auto runtime =
        co_await automation::AutomationRuntime::open(io.get_executor(),
                                                     automation::AutomationRuntimeOptions{.database_path = db});
    REQUIRE(runtime.has_value());
    REQUIRE((co_await seed_triggered_job(*runtime, "triggered:mock-main", "channel:mock-main")).has_value());
    auto service = runtime->automation_service();

    channel::ChannelManager manager{io.get_executor()};
    auto adapter =
        std::make_unique<channel::MockChannel>(io.get_executor(),
                                               channel::MockChannelOptions{.id = "mock-main", .kind = "mock"});
    auto* mock = adapter.get();
    REQUIRE(manager.register_adapter(std::move(adapter)).has_value());
    REQUIRE((co_await manager.start_all()).has_value());
    REQUIRE(mock->push_inbound(text_inbound("start triggered work")).has_value());

    std::vector<channel::ChannelPromptRunRequest> seen;
    auto runner = channel::ChannelPromptRunner{[&seen](channel::ChannelPromptRunRequest request)
                                                   -> async::Awaitable<core::Result<channel::ChannelPromptRunResult>> {
      seen.push_back(std::move(request));
      co_return channel::ChannelPromptRunResult{.text = "direct reply"};
    }};

    auto outcome = co_await bootstrap::serve_channels(
        io.get_executor(),
        manager,
        std::move(runner),
        std::vector<std::string>{"mock-main"},
        [&seen, mock] { return !seen.empty() && !mock->sent_messages().empty(); },
        &service);

    REQUIRE(outcome.has_value());
    REQUIRE(seen.size() == 1);
    CHECK(seen.front().channel_id == "mock-main");
    REQUIRE(mock->sent_messages().size() == 1);
    CHECK_FALSE(mock->started());
    CHECK(service.triggered_queue_size() == 1);
  });
}

TEST_CASE("serve_channels stops gracefully on parent cancellation", "[unit][bootstrap][serve][channels]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    channel::ChannelManager manager{io.get_executor()};
    auto adapter =
        std::make_unique<channel::MockChannel>(io.get_executor(),
                                               channel::MockChannelOptions{.id = "mock-main", .kind = "mock"});
    auto* mock = adapter.get();
    REQUIRE(manager.register_adapter(std::move(adapter)).has_value());
    REQUIRE((co_await manager.start_all()).has_value());
    REQUIRE(mock->started());

    int runner_calls{};
    auto runner = channel::ChannelPromptRunner{[&runner_calls](channel::ChannelPromptRunRequest)
                                                   -> async::Awaitable<core::Result<channel::ChannelPromptRunResult>> {
      ++runner_calls;
      co_return channel::ChannelPromptRunResult{.text = "unexpected"};
    }};

    using namespace asio::experimental::awaitable_operators;
    [[maybe_unused]] auto raced = co_await (bootstrap::serve_channels(io.get_executor(),
                                                                      manager,
                                                                      std::move(runner),
                                                                      std::vector<std::string>{"mock-main"}) ||
                                            async::sleep_for(io.get_executor(), 20ms));

    CHECK(runner_calls == 0);
    CHECK(mock->sent_messages().empty());
    CHECK_FALSE(mock->started());
  });
}

TEST_CASE("serve_channels rejects a null prompt runner", "[unit][bootstrap][serve][channels]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    channel::ChannelManager manager{io.get_executor()};
    auto outcome = co_await bootstrap::serve_channels(io.get_executor(), manager, {}, {});
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().kind() == core::ErrorKind::invalid_argument);
  });
}

TEST_CASE("serve_channels rejects invalid channel worker options", "[unit][bootstrap][serve][channels]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    channel::ChannelManager manager{io.get_executor()};
    auto runner = channel::ChannelPromptRunner{
        [](channel::ChannelPromptRunRequest) -> async::Awaitable<core::Result<channel::ChannelPromptRunResult>> {
          co_return channel::ChannelPromptRunResult{.text = "unused"};
        }};

    auto zero_capacity =
        co_await bootstrap::serve_channels(io.get_executor(),
                                           manager,
                                           runner,
                                           {},
                                           {},
                                           nullptr,
                                           bootstrap::ServeChannelOptions{.conversation_queue_capacity = 0});
    REQUIRE_FALSE(zero_capacity.has_value());
    CHECK(zero_capacity.error().kind() == core::ErrorKind::invalid_argument);

    auto zero_workers =
        co_await bootstrap::serve_channels(io.get_executor(),
                                           manager,
                                           runner,
                                           {},
                                           {},
                                           nullptr,
                                           bootstrap::ServeChannelOptions{.max_active_conversations = 0});
    REQUIRE_FALSE(zero_workers.has_value());
    CHECK(zero_workers.error().kind() == core::ErrorKind::invalid_argument);

    auto negative_ttl =
        co_await bootstrap::serve_channels(io.get_executor(),
                                           manager,
                                           std::move(runner),
                                           {},
                                           {},
                                           nullptr,
                                           bootstrap::ServeChannelOptions{.conversation_idle_ttl = -1ms});
    REQUIRE_FALSE(negative_ttl.has_value());
    CHECK(negative_ttl.error().kind() == core::ErrorKind::invalid_argument);

    auto zero_deadline = co_await bootstrap::serve_channels(io.get_executor(),
                                                            manager,
                                                            runner,
                                                            {},
                                                            {},
                                                            nullptr,
                                                            bootstrap::ServeChannelOptions{.message_deadline = 0ms});
    REQUIRE_FALSE(zero_deadline.has_value());
    CHECK(zero_deadline.error().kind() == core::ErrorKind::invalid_argument);

    auto negative_deadline =
        co_await bootstrap::serve_channels(io.get_executor(),
                                           manager,
                                           std::move(runner),
                                           {},
                                           {},
                                           nullptr,
                                           bootstrap::ServeChannelOptions{.message_deadline = -1ms});
    REQUIRE_FALSE(negative_deadline.has_value());
    CHECK(negative_deadline.error().kind() == core::ErrorKind::invalid_argument);
  });
}
