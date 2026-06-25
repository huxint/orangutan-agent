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
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>

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

[[nodiscard]] channel::InboundMessage text_inbound(std::string text) {
  return channel::InboundMessage{
      .channel_id = "mock-main",
      .conversation_id = "conv-1",
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
