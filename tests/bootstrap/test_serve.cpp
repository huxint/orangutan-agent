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

#include <chrono>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/automation.hpp>
#include <oran/bootstrap/serve.hpp>
#include <oran/core/error.hpp>
#include <oran/core/time.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace automation = orangutan::automation;
namespace bootstrap = orangutan::bootstrap;
namespace core = orangutan::core;
namespace test = orangutan::tests;

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
