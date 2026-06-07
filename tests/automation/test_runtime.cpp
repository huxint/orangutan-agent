// tests/automation/test_runtime.cpp - caller-owned automation runtime coverage.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/automation.hpp>
#include <oran/core/error.hpp>
#include <oran/hook.hpp>
#include <oran/memory/longterm.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace automation = orangutan::automation;
namespace core = orangutan::core;
namespace hook = orangutan::hook;
namespace memory = orangutan::memory;
namespace test = orangutan::tests;

namespace {

using namespace std::chrono_literals;

class TempWorkspace {
public:
  explicit TempWorkspace(std::string name)
      : path_{std::filesystem::temp_directory_path() /
              (std::move(name) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))} {}

  ~TempWorkspace() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  TempWorkspace(const TempWorkspace&) = delete;
  TempWorkspace& operator=(const TempWorkspace&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

template <typename Rep, typename Period>
[[nodiscard]] core::Time at(std::chrono::duration<Rep, Period> value) {
  return core::Time{core::Time::time_point{
      std::chrono::duration_cast<core::Time::clock::duration>(value),
  }};
}

[[nodiscard]] bool has_field(const core::Error& error, std::string_view field) {
  return std::ranges::any_of(error.context(),
                             [field](const auto& entry) { return entry.first == "field" && entry.second == field; });
}

[[nodiscard]] bool has_context(const core::Error& error, std::string_view key, std::string_view value) {
  return std::ranges::any_of(error.context(),
                             [key, value](const auto& entry) { return entry.first == key && entry.second == value; });
}

[[nodiscard]] std::string automation_db_path(const TempWorkspace& workspace) {
  return (workspace.path() / ".orangutan" / "automation.db").string();
}

automation::MemoryRetentionJob make_job(std::string scope_key = "cli") {
  return automation::MemoryRetentionJob{
      .scope_key = std::move(scope_key),
      .policy =
          automation::LongtermMemoryRetentionPolicy{
              .forget_after_unused = std::chrono::days{3},
              .importance_floor = 0.5,
              .max_records_per_scope = 7,
              .decay_check_interval = 6h,
          },
      .first_fire_at = at(60s),
  };
}

automation::CronSchedule make_cron_schedule(std::string expression = "* * * * *") {
  return automation::CronSchedule{
      .expression = std::move(expression),
      .first_fire_at = at(60s),
  };
}

memory::longterm::Record make_record(std::string id) {
  return memory::longterm::Record{
      .key = memory::longterm::RecordKey{.id = std::move(id), .scope_key = "cli"},
      .kind = memory::longterm::RecordKind::project,
      .title = "title",
      .body = "body",
      .created_at = at(1s),
      .updated_at = at(1s),
      .last_read_at = at(1s),
      .importance = 0.1,
      .tags = {},
      .linked_record_ids = {},
      .shadow = true,
  };
}

class FakeBackend final : public memory::longterm::Backend {
public:
  [[nodiscard]] async::Awaitable<core::Result<memory::longterm::Record>> get(memory::longterm::RecordKey) override {
    co_return std::unexpected(core::Error::internal("unused backend operation"));
  }

  [[nodiscard]] async::Awaitable<core::Result<std::vector<memory::longterm::SearchHit>>> search(memory::longterm::Query,
                                                                                                std::size_t) override {
    co_return std::unexpected(core::Error::internal("unused backend operation"));
  }

  [[nodiscard]] async::Awaitable<core::Result<memory::longterm::Record>>
  upsert(memory::longterm::WriteRequest) override {
    co_return std::unexpected(core::Error::internal("unused backend operation"));
  }

  [[nodiscard]] async::Awaitable<core::Result<memory::longterm::Record>>
  touch(memory::longterm::TouchRequest) override {
    co_return std::unexpected(core::Error::internal("unused backend operation"));
  }

  [[nodiscard]] async::Awaitable<core::Result<memory::longterm::DecayResult>>
  decay(memory::longterm::DecayRequest request) override {
    ++decay_calls;
    last_decay = std::move(request);
    if (decay_error) {
      co_return std::unexpected(*decay_error);
    }
    co_return decay_result;
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> remove(memory::longterm::RecordKey) override {
    co_return std::unexpected(core::Error::internal("unused backend operation"));
  }

  int decay_calls{};
  memory::longterm::DecayRequest last_decay{};
  memory::longterm::DecayResult decay_result{};
  std::optional<core::Error> decay_error{};
};

}  // namespace

TEST_CASE("AutomationRuntime::open creates parent directories and migrates state", "[unit][automation][runtime]") {
  TempWorkspace workspace{"oran-automation-runtime-open"};
  test::run_async([&workspace](asio::io_context& io) -> async::Awaitable<void> {
    const auto db_path = automation_db_path(workspace);
    REQUIRE_FALSE(std::filesystem::exists(workspace.path() / ".orangutan"));

    auto opened = co_await automation::AutomationRuntime::open(io.get_executor(),
                                                               automation::AutomationRuntimeOptions{
                                                                   .database_path = db_path,
                                                                   .reader_count = 1,
                                                                   .statement_cache_capacity = 4,
                                                               });

    REQUIRE(opened.has_value());
    REQUIRE(opened->database_path() == db_path);
    REQUIRE(std::filesystem::exists(workspace.path() / ".orangutan" / "automation.db"));
    REQUIRE(opened->migration_report().previous_version == 0);
    REQUIRE(opened->migration_report().current_version == 5);
    REQUIRE(opened->migration_report().applied_versions == std::vector<std::int64_t>{1, 2, 3, 4, 5});

    auto upserted =
        co_await opened->repository().upsert_memory_retention_job(automation::UpsertMemoryRetentionJobRequest{
            .job_key = "memory-retention:cli",
            .job = make_job(),
        });
    REQUIRE(upserted.has_value());

    auto loaded = co_await opened->repository().get_memory_retention_job("memory-retention:cli");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    REQUIRE((*loaded)->job == make_job());
  });
}

TEST_CASE("AutomationRuntime::open reuses an already migrated automation database", "[unit][automation][runtime]") {
  TempWorkspace workspace{"oran-automation-runtime-reopen"};
  test::run_async([&workspace](asio::io_context& io) -> async::Awaitable<void> {
    const auto db_path = automation_db_path(workspace);
    auto first =
        co_await automation::AutomationRuntime::open(io.get_executor(),
                                                     automation::AutomationRuntimeOptions{.database_path = db_path});
    REQUIRE(first.has_value());
    REQUIRE(first->migration_report().previous_version == 0);
    REQUIRE(first->migration_report().current_version == 5);

    auto second =
        co_await automation::AutomationRuntime::open(io.get_executor(),
                                                     automation::AutomationRuntimeOptions{.database_path = db_path});
    REQUIRE(second.has_value());
    REQUIRE(second->migration_report().previous_version == 5);
    REQUIRE(second->migration_report().current_version == 5);
    REQUIRE(second->migration_report().applied_versions.empty());
  });
}

TEST_CASE("AutomationRuntime::open rejects an empty database path", "[unit][automation][runtime]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    auto opened =
        co_await automation::AutomationRuntime::open(io.get_executor(), automation::AutomationRuntimeOptions{});

    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(has_field(opened.error(), "database_path"));
  });
}

TEST_CASE("AutomationRuntime applies cron job seeds explicitly", "[unit][automation][runtime][cron]") {
  TempWorkspace workspace{"oran-automation-runtime-cron-seeds"};
  test::run_async([&workspace](asio::io_context& io) -> async::Awaitable<void> {
    auto runtime = co_await automation::AutomationRuntime::open(
        io.get_executor(),
        automation::AutomationRuntimeOptions{.database_path = automation_db_path(workspace)});
    REQUIRE(runtime.has_value());

    auto seeds = std::vector<automation::UpsertCronJobRequest>{
        automation::UpsertCronJobRequest{
            .job_key = "cron:daily-summary",
            .schedule =
                automation::CronSchedule{
                    .expression = "0 9 * * *",
                    .first_fire_at = at(60s),
                },
        },
        automation::UpsertCronJobRequest{
            .job_key = "cron:hourly-ci",
            .schedule =
                automation::CronSchedule{
                    .expression = "15 * * * *",
                    .first_fire_at = at(120s),
                },
            .state =
                automation::PeriodicJobState{
                    .last_fired_at = at(300s),
                },
        },
    };

    auto applied = co_await runtime->apply_cron_job_seeds(seeds);

    REQUIRE(applied.has_value());
    REQUIRE(applied->requested_count == 2);
    REQUIRE(applied->upserted_count == 2);
    REQUIRE(applied->jobs.size() == 2);
    REQUIRE(applied->jobs[0].job_key == "cron:daily-summary");
    REQUIRE(applied->jobs[1].state.last_fired_at == at(300s));

    seeds[0].state.last_fired_at = at(600s);
    auto reapplied = co_await runtime->apply_cron_job_seeds(seeds);
    REQUIRE(reapplied.has_value());
    REQUIRE(reapplied->upserted_count == 2);

    auto loaded = co_await runtime->repository().get_cron_job("cron:daily-summary");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    REQUIRE((*loaded)->state.last_fired_at == at(600s));

    auto bad_seeds = std::vector<automation::UpsertCronJobRequest>{
        automation::UpsertCronJobRequest{
            .job_key = "cron:bad",
            .schedule =
                automation::CronSchedule{
                    .expression = "not a cron",
                    .first_fire_at = at(60s),
                },
        },
    };
    auto invalid = co_await runtime->apply_cron_job_seeds(bad_seeds);
    REQUIRE_FALSE(invalid.has_value());
    REQUIRE(invalid.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(has_context(invalid.error(), "seed_index", "0"));
    REQUIRE(has_context(invalid.error(), "job_key", "cron:bad"));
  });
}

TEST_CASE("AutomationRuntime runs a caller-awaited cron service cycle", "[unit][automation][runtime][cron][service]") {
  TempWorkspace workspace{"oran-automation-runtime-cron-service-cycle"};
  test::run_async([&workspace](asio::io_context& io) -> async::Awaitable<void> {
    auto runtime = co_await automation::AutomationRuntime::open(
        io.get_executor(),
        automation::AutomationRuntimeOptions{.database_path = automation_db_path(workspace)});
    REQUIRE(runtime.has_value());

    std::vector<core::Time> fired_at;
    auto seeds = std::vector<automation::UpsertCronJobRequest>{
        automation::UpsertCronJobRequest{
            .job_key = "cron:every-minute",
            .schedule = make_cron_schedule(),
        },
    };
    auto result = co_await runtime->run_cron_service_cycle(automation::CronServiceCycleRequest{
        .seeds = std::move(seeds),
        .now = at(120s),
        .max_iterations = 3,
        .job_limit = 10,
        .handler = [&fired_at](automation::CronDueJob due) -> async::Awaitable<core::Result<void>> {
          fired_at.push_back(due.schedule.next_fire_at);
          co_return core::Result<void>{};
        },
    });

    REQUIRE(result.has_value());
    REQUIRE(result->seed_apply.requested_count == 1);
    REQUIRE(result->seed_apply.upserted_count == 1);
    REQUIRE(result->loop.iterations == 3);
    REQUIRE(result->loop.attempted_count == 2);
    REQUIRE(result->loop.advanced_count == 2);
    REQUIRE(result->loop.failed_count == 0);
    REQUIRE(result->loop.stop_reason == automation::CronLoopRunStopReason::no_due_work);
    REQUIRE(fired_at == std::vector{at(60s), at(120s)});

    auto loaded = co_await runtime->repository().get_cron_job("cron:every-minute");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    REQUIRE((*loaded)->state.last_fired_at == at(120s));
  });
}

TEST_CASE("AutomationRuntime forwards cron service cycle stop requests", "[unit][automation][runtime][cron][service]") {
  TempWorkspace workspace{"oran-automation-runtime-cron-service-cycle-stop"};
  test::run_async([&workspace](asio::io_context& io) -> async::Awaitable<void> {
    auto runtime = co_await automation::AutomationRuntime::open(
        io.get_executor(),
        automation::AutomationRuntimeOptions{.database_path = automation_db_path(workspace)});
    REQUIRE(runtime.has_value());

    std::size_t handler_calls{};
    auto seeds = std::vector<automation::UpsertCronJobRequest>{
        automation::UpsertCronJobRequest{
            .job_key = "cron:every-minute",
            .schedule = make_cron_schedule(),
        },
    };
    auto result = co_await runtime->run_cron_service_cycle(automation::CronServiceCycleRequest{
        .seeds = std::move(seeds),
        .now = at(120s),
        .max_iterations = 5,
        .job_limit = 10,
        .handler = [&handler_calls](automation::CronDueJob) -> async::Awaitable<core::Result<void>> {
          ++handler_calls;
          co_return core::Result<void>{};
        },
        .stop_requested = [&handler_calls] { return handler_calls >= 1; },
    });

    REQUIRE(result.has_value());
    REQUIRE(result->seed_apply.requested_count == 1);
    REQUIRE(result->seed_apply.upserted_count == 1);
    REQUIRE(result->loop.iterations == 1);
    REQUIRE(result->loop.attempted_count == 1);
    REQUIRE(result->loop.advanced_count == 1);
    REQUIRE(result->loop.failed_count == 0);
    REQUIRE(result->loop.stop_reason == automation::CronLoopRunStopReason::stop_requested);
    REQUIRE(handler_calls == 1);
  });
}

TEST_CASE("AutomationRuntime validates cron service cycles before applying seeds",
          "[unit][automation][runtime][cron][service]") {
  TempWorkspace workspace{"oran-automation-runtime-cron-service-cycle-invalid"};
  test::run_async([&workspace](asio::io_context& io) -> async::Awaitable<void> {
    auto runtime = co_await automation::AutomationRuntime::open(
        io.get_executor(),
        automation::AutomationRuntimeOptions{.database_path = automation_db_path(workspace)});
    REQUIRE(runtime.has_value());

    auto seeds = std::vector<automation::UpsertCronJobRequest>{
        automation::UpsertCronJobRequest{
            .job_key = "cron:never-applied",
            .schedule = make_cron_schedule(),
        },
    };
    auto result = co_await runtime->run_cron_service_cycle(automation::CronServiceCycleRequest{
        .seeds = std::move(seeds),
        .now = at(120s),
        .max_iterations = 0,
        .job_limit = 10,
        .handler = [](automation::CronDueJob) -> async::Awaitable<core::Result<void>> {
          co_return core::Result<void>{};
        },
    });

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(has_field(result.error(), "max_iterations"));

    auto loaded = co_await runtime->repository().get_cron_job("cron:never-applied");
    REQUIRE(loaded.has_value());
    REQUIRE_FALSE(loaded->has_value());
  });
}

TEST_CASE("AutomationRuntime creates cron services over owned repository state", "[unit][automation][runtime][cron]") {
  TempWorkspace workspace{"oran-automation-runtime-cron-service"};
  test::run_async([&workspace](asio::io_context& io) -> async::Awaitable<void> {
    auto runtime = co_await automation::AutomationRuntime::open(
        io.get_executor(),
        automation::AutomationRuntimeOptions{.database_path = automation_db_path(workspace)});
    REQUIRE(runtime.has_value());
    REQUIRE((co_await runtime->repository().upsert_cron_job(automation::UpsertCronJobRequest{
                 .job_key = "cron:every-minute",
                 .schedule = make_cron_schedule(),
             }))
                .has_value());
    auto five_minute = make_cron_schedule("*/5 * * * *");
    five_minute.first_fire_at = at(300s);
    REQUIRE((co_await runtime->repository().upsert_cron_job(automation::UpsertCronJobRequest{
                 .job_key = "cron:five-minute",
                 .schedule = five_minute,
             }))
                .has_value());

    auto service = runtime->cron_service();
    auto ticked = co_await service.tick(automation::CronTickRequest{
        .now = at(120s),
        .job_limit = 10,
    });

    REQUIRE(ticked.has_value());
    REQUIRE(ticked->checked_count == 2);
    REQUIRE(ticked->due_jobs.size() == 1);
    REQUIRE(ticked->due_jobs[0].job.job_key == "cron:every-minute");
    REQUIRE(ticked->due_jobs[0].schedule.next_fire_at == at(60s));
    REQUIRE(ticked->due_jobs[0].schedule.overdue_by == 60s);
    REQUIRE(ticked->next_fire_at == at(60s));

    auto loaded = co_await runtime->repository().get_cron_job("cron:every-minute");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    REQUIRE_FALSE((*loaded)->state.last_fired_at.has_value());
  });
}

TEST_CASE("CronLoop::run_once skips waits beyond the caller budget", "[unit][automation][runtime][cron][loop]") {
  TempWorkspace workspace{"oran-automation-runtime-cron-loop-budget"};
  test::run_async([&workspace](asio::io_context& io) -> async::Awaitable<void> {
    auto runtime = co_await automation::AutomationRuntime::open(
        io.get_executor(),
        automation::AutomationRuntimeOptions{.database_path = automation_db_path(workspace)});
    REQUIRE(runtime.has_value());
    auto schedule = make_cron_schedule("*/5 * * * *");
    schedule.first_fire_at = at(300s);
    REQUIRE((co_await runtime->repository().upsert_cron_job(automation::UpsertCronJobRequest{
                 .job_key = "cron:five-minute",
                 .schedule = schedule,
             }))
                .has_value());

    auto loop = runtime->cron_loop();
    auto result = co_await loop.run_once(automation::CronLoopRunOnceRequest{
        .now = at(0s),
        .max_wait = 10ms,
        .job_limit = 10,
    });

    REQUIRE(result.has_value());
    REQUIRE(result->waited_for == 0ns);
    REQUIRE(result->tick.checked_count == 1);
    REQUIRE(result->tick.due_jobs.empty());
    REQUIRE(result->tick.next_fire_at == at(300s));
  });
}

TEST_CASE("CronLoop::run_once waits within budget and reports due cron jobs",
          "[unit][automation][runtime][cron][loop]") {
  TempWorkspace workspace{"oran-automation-runtime-cron-loop-wait"};
  test::run_async([&workspace](asio::io_context& io) -> async::Awaitable<void> {
    auto runtime = co_await automation::AutomationRuntime::open(
        io.get_executor(),
        automation::AutomationRuntimeOptions{.database_path = automation_db_path(workspace)});
    REQUIRE(runtime.has_value());
    REQUIRE((co_await runtime->repository().upsert_cron_job(automation::UpsertCronJobRequest{
                 .job_key = "cron:every-minute",
                 .schedule = make_cron_schedule(),
             }))
                .has_value());

    auto loop = runtime->cron_loop();
    auto result = co_await loop.run_once(automation::CronLoopRunOnceRequest{
        .now = at(59s + 990ms),
        .max_wait = 50ms,
        .job_limit = 10,
    });

    REQUIRE(result.has_value());
    REQUIRE(result->waited_for == 10ms);
    REQUIRE(result->tick.checked_count == 1);
    REQUIRE(result->tick.due_jobs.size() == 1);
    REQUIRE(result->tick.due_jobs[0].job.job_key == "cron:every-minute");
    REQUIRE(result->tick.due_jobs[0].schedule.next_fire_at == at(60s));

    auto loaded = co_await runtime->repository().get_cron_job("cron:every-minute");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    REQUIRE_FALSE((*loaded)->state.last_fired_at.has_value());
  });
}

TEST_CASE("CronLoop::run catches up due cron fires through the supplied handler",
          "[unit][automation][runtime][cron][loop]") {
  TempWorkspace workspace{"oran-automation-runtime-cron-loop-run-catch-up"};
  test::run_async([&workspace](asio::io_context& io) -> async::Awaitable<void> {
    auto runtime = co_await automation::AutomationRuntime::open(
        io.get_executor(),
        automation::AutomationRuntimeOptions{.database_path = automation_db_path(workspace)});
    REQUIRE(runtime.has_value());
    REQUIRE((co_await runtime->repository().upsert_cron_job(automation::UpsertCronJobRequest{
                 .job_key = "cron:every-minute",
                 .schedule = make_cron_schedule(),
             }))
                .has_value());

    std::vector<core::Time> fired_at;
    auto loop = runtime->cron_loop();
    auto result = co_await loop.run(automation::CronLoopRunRequest{
        .now = at(120s),
        .max_iterations = 3,
        .job_limit = 10,
        .handler = [&fired_at](automation::CronDueJob due) -> async::Awaitable<core::Result<void>> {
          fired_at.push_back(due.schedule.next_fire_at);
          co_return core::Result<void>{};
        },
    });

    REQUIRE(result.has_value());
    REQUIRE(result->iterations == 3);
    REQUIRE(result->attempted_count == 2);
    REQUIRE(result->advanced_count == 2);
    REQUIRE(result->failed_count == 0);
    REQUIRE(result->waited_for == 0ns);
    REQUIRE(result->stop_reason == automation::CronLoopRunStopReason::no_due_work);
    REQUIRE(result->last_execution.has_value());
    REQUIRE(result->last_execution->tick.due_jobs.empty());
    REQUIRE(fired_at == std::vector{at(60s), at(120s)});

    auto loaded = co_await runtime->repository().get_cron_job("cron:every-minute");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    REQUIRE((*loaded)->state.last_fired_at == at(120s));
  });
}

TEST_CASE("CronLoop::run honors stop requests before starting work", "[unit][automation][runtime][cron][loop]") {
  TempWorkspace workspace{"oran-automation-runtime-cron-loop-run-stop-before"};
  test::run_async([&workspace](asio::io_context& io) -> async::Awaitable<void> {
    auto runtime = co_await automation::AutomationRuntime::open(
        io.get_executor(),
        automation::AutomationRuntimeOptions{.database_path = automation_db_path(workspace)});
    REQUIRE(runtime.has_value());
    REQUIRE((co_await runtime->repository().upsert_cron_job(automation::UpsertCronJobRequest{
                 .job_key = "cron:every-minute",
                 .schedule = make_cron_schedule(),
             }))
                .has_value());

    int handler_calls{};
    auto loop = runtime->cron_loop();
    auto result = co_await loop.run(automation::CronLoopRunRequest{
        .now = at(120s),
        .max_iterations = 5,
        .job_limit = 10,
        .handler = [&handler_calls](automation::CronDueJob) -> async::Awaitable<core::Result<void>> {
          ++handler_calls;
          co_return core::Result<void>{};
        },
        .stop_requested = [] { return true; },
    });

    REQUIRE(result.has_value());
    REQUIRE(result->iterations == 0);
    REQUIRE(result->attempted_count == 0);
    REQUIRE(result->advanced_count == 0);
    REQUIRE(result->failed_count == 0);
    REQUIRE(result->stop_reason == automation::CronLoopRunStopReason::stop_requested);
    REQUIRE_FALSE(result->last_execution.has_value());
    REQUIRE(handler_calls == 0);

    auto loaded = co_await runtime->repository().get_cron_job("cron:every-minute");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    REQUIRE_FALSE((*loaded)->state.last_fired_at.has_value());

    auto runs = co_await runtime->repository().list_cron_runs(automation::ListCronRunsOptions{
        .job_key = "cron:every-minute",
        .limit = 10,
    });
    REQUIRE(runs.has_value());
    REQUIRE(runs->empty());
  });
}

TEST_CASE("CronLoop::run stops after a successful iteration when requested",
          "[unit][automation][runtime][cron][loop]") {
  TempWorkspace workspace{"oran-automation-runtime-cron-loop-run-stop-after"};
  test::run_async([&workspace](asio::io_context& io) -> async::Awaitable<void> {
    auto runtime = co_await automation::AutomationRuntime::open(
        io.get_executor(),
        automation::AutomationRuntimeOptions{.database_path = automation_db_path(workspace)});
    REQUIRE(runtime.has_value());
    REQUIRE((co_await runtime->repository().upsert_cron_job(automation::UpsertCronJobRequest{
                 .job_key = "cron:every-minute",
                 .schedule = make_cron_schedule(),
             }))
                .has_value());

    std::vector<core::Time> fired_at;
    auto loop = runtime->cron_loop();
    auto result = co_await loop.run(automation::CronLoopRunRequest{
        .now = at(120s),
        .max_iterations = 5,
        .job_limit = 10,
        .handler = [&fired_at](automation::CronDueJob due) -> async::Awaitable<core::Result<void>> {
          fired_at.push_back(due.schedule.next_fire_at);
          co_return core::Result<void>{};
        },
        .stop_requested = [&fired_at] { return !fired_at.empty(); },
    });

    REQUIRE(result.has_value());
    REQUIRE(result->iterations == 1);
    REQUIRE(result->attempted_count == 1);
    REQUIRE(result->advanced_count == 1);
    REQUIRE(result->failed_count == 0);
    REQUIRE(result->stop_reason == automation::CronLoopRunStopReason::stop_requested);
    REQUIRE(result->last_execution.has_value());
    REQUIRE(result->last_execution->attempts.size() == 1);
    REQUIRE(result->last_execution->attempts[0].run.has_value());
    REQUIRE(result->last_execution->attempts[0].run->success);
    REQUIRE(fired_at == std::vector{at(60s)});

    auto loaded = co_await runtime->repository().get_cron_job("cron:every-minute");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    REQUIRE((*loaded)->state.last_fired_at == at(60s));
  });
}

TEST_CASE("CronLoop::run stops on handler failure without retrying immediately",
          "[unit][automation][runtime][cron][loop]") {
  TempWorkspace workspace{"oran-automation-runtime-cron-loop-run-handler-failure"};
  test::run_async([&workspace](asio::io_context& io) -> async::Awaitable<void> {
    auto runtime = co_await automation::AutomationRuntime::open(
        io.get_executor(),
        automation::AutomationRuntimeOptions{.database_path = automation_db_path(workspace)});
    REQUIRE(runtime.has_value());
    REQUIRE((co_await runtime->repository().upsert_cron_job(automation::UpsertCronJobRequest{
                 .job_key = "cron:fails",
                 .schedule = make_cron_schedule(),
             }))
                .has_value());

    int handler_calls{};
    auto loop = runtime->cron_loop();
    auto result = co_await loop.run(automation::CronLoopRunRequest{
        .now = at(120s),
        .max_iterations = 5,
        .job_limit = 10,
        .handler = [&handler_calls](automation::CronDueJob) -> async::Awaitable<core::Result<void>> {
          ++handler_calls;
          co_return std::unexpected(core::Error::upstream("cron payload failed"));
        },
    });

    REQUIRE(result.has_value());
    REQUIRE(result->iterations == 1);
    REQUIRE(result->attempted_count == 1);
    REQUIRE(result->advanced_count == 0);
    REQUIRE(result->failed_count == 1);
    REQUIRE(result->stop_reason == automation::CronLoopRunStopReason::handler_failure);
    REQUIRE(result->last_execution.has_value());
    REQUIRE(result->last_execution->attempts.size() == 1);
    REQUIRE(result->last_execution->attempts[0].error.has_value());
    REQUIRE(result->last_execution->attempts[0].error->kind() == core::ErrorKind::upstream);
    REQUIRE(handler_calls == 1);

    auto loaded = co_await runtime->repository().get_cron_job("cron:fails");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    REQUIRE_FALSE((*loaded)->state.last_fired_at.has_value());
  });
}

TEST_CASE("AutomationRuntime creates retention services over owned repository state", "[unit][automation][runtime]") {
  TempWorkspace workspace{"oran-automation-runtime-service"};
  test::run_async([&workspace](asio::io_context& io) -> async::Awaitable<void> {
    auto runtime = co_await automation::AutomationRuntime::open(
        io.get_executor(),
        automation::AutomationRuntimeOptions{.database_path = automation_db_path(workspace)});
    REQUIRE(runtime.has_value());
    REQUIRE((co_await runtime->repository().upsert_memory_retention_job(automation::UpsertMemoryRetentionJobRequest{
                 .job_key = "memory-retention:cli",
                 .job = make_job(),
             }))
                .has_value());

    FakeBackend backend;
    backend.decay_result.shadowed_records = {make_record("rec-1")};
    std::vector<hook::MemoryDecayPayload> captured;
    hook::Bus bus;
    hook::InProcessSink sink{
        "runtime-retention-recorder",
        [&captured](hook::Event event, hook::PayloadPtr payload) -> async::Awaitable<core::Result<void>> {
          REQUIRE(event == hook::Event::memory_decay);
          const auto* decay = std::get_if<hook::MemoryDecayPayload>(payload.get());
          REQUIRE(decay != nullptr);
          captured.push_back(*decay);
          co_return core::Result<void>{};
        }};
    bus.bind(sink, {hook::Event::memory_decay});
    auto service = runtime->memory_retention_service(backend,
                                                     automation::MemoryRetentionServiceOptions{
                                                         .hooks =
                                                             automation::MemoryRetentionHookOptions{
                                                                 .bus = &bus,
                                                                 .source = "periodic",
                                                                 .agent_key = "automation-runtime",
                                                                 .identity = "retention-service",
                                                             },
                                                     });

    auto ticked = co_await service.tick(automation::MemoryRetentionTickRequest{
        .job_key = "memory-retention:cli",
        .now = at(60s),
    });

    REQUIRE(ticked.has_value());
    REQUIRE(ticked->ran);
    REQUIRE(ticked->shadowed_count == 1);
    REQUIRE(backend.decay_calls == 1);
    REQUIRE(captured.size() == 1);
    REQUIRE(captured[0].who.agent_key == "automation-runtime");

    auto loaded = co_await runtime->repository().get_memory_retention_job("memory-retention:cli");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    REQUIRE((*loaded)->state.last_fired_at == at(60s));
  });
}

TEST_CASE("AutomationRuntime creates a retention loop that skips waits beyond budget",
          "[unit][automation][runtime][loop]") {
  TempWorkspace workspace{"oran-automation-runtime-loop-budget"};
  test::run_async([&workspace](asio::io_context& io) -> async::Awaitable<void> {
    auto runtime = co_await automation::AutomationRuntime::open(
        io.get_executor(),
        automation::AutomationRuntimeOptions{.database_path = automation_db_path(workspace)});
    REQUIRE(runtime.has_value());
    REQUIRE((co_await runtime->repository().upsert_memory_retention_job(automation::UpsertMemoryRetentionJobRequest{
                 .job_key = "memory-retention:cli",
                 .job = make_job(),
             }))
                .has_value());

    FakeBackend backend;
    auto loop = runtime->memory_retention_loop(backend);
    auto result = co_await loop.run_once(automation::MemoryRetentionLoopRunOnceRequest{
        .job_key = "memory-retention:cli",
        .now = at(0s),
        .max_wait = 10ms,
    });

    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->tick.ran);
    REQUIRE(result->tick.schedule.next_fire_at == at(60s));
    REQUIRE(result->waited_for == 0ns);
    REQUIRE(backend.decay_calls == 0);
  });
}

TEST_CASE("AutomationRuntime retention loop waits within budget and runs due work",
          "[unit][automation][runtime][loop]") {
  TempWorkspace workspace{"oran-automation-runtime-loop-wait"};
  test::run_async([&workspace](asio::io_context& io) -> async::Awaitable<void> {
    auto runtime = co_await automation::AutomationRuntime::open(
        io.get_executor(),
        automation::AutomationRuntimeOptions{.database_path = automation_db_path(workspace)});
    REQUIRE(runtime.has_value());
    REQUIRE((co_await runtime->repository().upsert_memory_retention_job(automation::UpsertMemoryRetentionJobRequest{
                 .job_key = "memory-retention:cli",
                 .job = make_job(),
             }))
                .has_value());

    FakeBackend backend;
    backend.decay_result.shadowed_records = {make_record("rec-1"), make_record("rec-2")};
    auto loop = runtime->memory_retention_loop(backend);
    auto result = co_await loop.run_once(automation::MemoryRetentionLoopRunOnceRequest{
        .job_key = "memory-retention:cli",
        .now = at(59s + 990ms),
        .max_wait = 50ms,
    });

    REQUIRE(result.has_value());
    REQUIRE(result->tick.ran);
    REQUIRE(result->waited_for == 10ms);
    REQUIRE(result->tick.shadowed_count == 2);
    REQUIRE(backend.decay_calls == 1);
    REQUIRE(backend.last_decay.decay_at == at(60s));

    auto loaded = co_await runtime->repository().get_memory_retention_job("memory-retention:cli");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    REQUIRE((*loaded)->state.last_fired_at == at(60s));

    auto reacquired =
        co_await runtime->repository().acquire_memory_retention_lease(automation::AcquireMemoryRetentionLeaseRequest{
            .job_key = "memory-retention:cli",
            .owner_key = "post-loop-owner",
            .acquired_at = at(61s),
            .expires_at = at(62s),
        });
    REQUIRE(reacquired.has_value());
    REQUIRE(reacquired->has_value());
    REQUIRE((*reacquired)->owner_key == "post-loop-owner");
    REQUIRE(
        (co_await runtime->repository().release_memory_retention_lease(automation::ReleaseMemoryRetentionLeaseRequest{
             .job_key = "memory-retention:cli",
             .owner_key = "post-loop-owner",
         }))
            .value());
  });
}

TEST_CASE("MemoryRetentionLoop::run drives finite due backlog without hidden service ownership",
          "[unit][automation][runtime][loop]") {
  TempWorkspace workspace{"oran-automation-runtime-loop-run-backlog"};
  test::run_async([&workspace](asio::io_context& io) -> async::Awaitable<void> {
    auto runtime = co_await automation::AutomationRuntime::open(
        io.get_executor(),
        automation::AutomationRuntimeOptions{.database_path = automation_db_path(workspace)});
    REQUIRE(runtime.has_value());
    REQUIRE((co_await runtime->repository().upsert_memory_retention_job(automation::UpsertMemoryRetentionJobRequest{
                 .job_key = "memory-retention:cli",
                 .job = make_job(),
                 .state = automation::PeriodicJobState{.last_fired_at = at(60s)},
             }))
                .has_value());

    FakeBackend backend;
    backend.decay_result.shadowed_records = {make_record("rec-1")};
    auto loop = runtime->memory_retention_loop(backend);
    auto result = co_await loop.run(automation::MemoryRetentionLoopRunRequest{
        .job_key = "memory-retention:cli",
        .now = at(60s + 18h + 1s),
        .max_total_wait = 0ms,
        .max_iterations = 3,
        .lease_owner_key = "loop-run-owner",
        .lease_ttl = 30s,
    });

    REQUIRE(result.has_value());
    REQUIRE(result->iterations == 3);
    REQUIRE(result->due_runs == 3);
    REQUIRE(result->waited_for == 0ns);
    REQUIRE(result->stop_reason == automation::MemoryRetentionLoopRunStopReason::iteration_limit);
    REQUIRE(result->last_step.has_value());
    REQUIRE(result->last_step->tick.ran);
    REQUIRE(result->last_step->tick.schedule.next_fire_at == at(60s + 18h));
    REQUIRE(backend.decay_calls == 3);

    auto loaded = co_await runtime->repository().get_memory_retention_job("memory-retention:cli");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    REQUIRE((*loaded)->state.last_fired_at == at(60s + 18h));

    auto runs = co_await runtime->repository().list_memory_retention_runs(automation::ListMemoryRetentionRunsOptions{
        .job_key = "memory-retention:cli",
        .limit = 10,
    });
    REQUIRE(runs.has_value());
    REQUIRE(runs->size() == 3);
  });
}

TEST_CASE("MemoryRetentionLoop::run stops when the next fire exceeds the caller wait budget",
          "[unit][automation][runtime][loop]") {
  TempWorkspace workspace{"oran-automation-runtime-loop-run-budget"};
  test::run_async([&workspace](asio::io_context& io) -> async::Awaitable<void> {
    auto runtime = co_await automation::AutomationRuntime::open(
        io.get_executor(),
        automation::AutomationRuntimeOptions{.database_path = automation_db_path(workspace)});
    REQUIRE(runtime.has_value());
    REQUIRE((co_await runtime->repository().upsert_memory_retention_job(automation::UpsertMemoryRetentionJobRequest{
                 .job_key = "memory-retention:cli",
                 .job = make_job(),
             }))
                .has_value());

    FakeBackend backend;
    auto loop = runtime->memory_retention_loop(backend);
    auto result = co_await loop.run(automation::MemoryRetentionLoopRunRequest{
        .job_key = "memory-retention:cli",
        .now = at(0s),
        .max_total_wait = 10ms,
        .max_iterations = 5,
    });

    REQUIRE(result.has_value());
    REQUIRE(result->iterations == 1);
    REQUIRE(result->due_runs == 0);
    REQUIRE(result->waited_for == 0ns);
    REQUIRE(result->stop_reason == automation::MemoryRetentionLoopRunStopReason::no_due_work);
    REQUIRE(result->last_step.has_value());
    REQUIRE_FALSE(result->last_step->tick.ran);
    REQUIRE(result->last_step->tick.schedule.next_fire_at == at(60s));
    REQUIRE(backend.decay_calls == 0);
  });
}

TEST_CASE("MemoryRetentionLoop::run_once rejects a job with an active lease",
          "[unit][automation][runtime][loop][lease]") {
  TempWorkspace workspace{"oran-automation-runtime-loop-lease"};
  test::run_async([&workspace](asio::io_context& io) -> async::Awaitable<void> {
    auto runtime = co_await automation::AutomationRuntime::open(
        io.get_executor(),
        automation::AutomationRuntimeOptions{.database_path = automation_db_path(workspace)});
    REQUIRE(runtime.has_value());
    REQUIRE((co_await runtime->repository().upsert_memory_retention_job(automation::UpsertMemoryRetentionJobRequest{
                 .job_key = "memory-retention:cli",
                 .job = make_job(),
             }))
                .has_value());
    auto held =
        co_await runtime->repository().acquire_memory_retention_lease(automation::AcquireMemoryRetentionLeaseRequest{
            .job_key = "memory-retention:cli",
            .owner_key = "existing-owner",
            .acquired_at = at(50s),
            .expires_at = at(120s),
        });
    REQUIRE(held.has_value());
    REQUIRE(held->has_value());

    FakeBackend backend;
    auto loop = runtime->memory_retention_loop(backend);
    auto result = co_await loop.run_once(automation::MemoryRetentionLoopRunOnceRequest{
        .job_key = "memory-retention:cli",
        .now = at(60s),
        .max_wait = 0ms,
        .lease_owner_key = "loop-owner",
        .lease_ttl = 30s,
    });

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::conflict);
    REQUIRE(backend.decay_calls == 0);

    auto still_held =
        co_await runtime->repository().acquire_memory_retention_lease(automation::AcquireMemoryRetentionLeaseRequest{
            .job_key = "memory-retention:cli",
            .owner_key = "third-owner",
            .acquired_at = at(61s),
            .expires_at = at(130s),
        });
    REQUIRE(still_held.has_value());
    REQUIRE_FALSE(still_held->has_value());
  });
}

TEST_CASE("MemoryRetentionLoop::run_once releases a due lease after backend failure",
          "[unit][automation][runtime][loop][lease]") {
  TempWorkspace workspace{"oran-automation-runtime-loop-release-on-error"};
  test::run_async([&workspace](asio::io_context& io) -> async::Awaitable<void> {
    auto runtime = co_await automation::AutomationRuntime::open(
        io.get_executor(),
        automation::AutomationRuntimeOptions{.database_path = automation_db_path(workspace)});
    REQUIRE(runtime.has_value());
    REQUIRE((co_await runtime->repository().upsert_memory_retention_job(automation::UpsertMemoryRetentionJobRequest{
                 .job_key = "memory-retention:cli",
                 .job = make_job(),
             }))
                .has_value());

    FakeBackend backend;
    backend.decay_error = core::Error::upstream("backend unavailable");
    auto loop = runtime->memory_retention_loop(backend);
    auto result = co_await loop.run_once(automation::MemoryRetentionLoopRunOnceRequest{
        .job_key = "memory-retention:cli",
        .now = at(60s),
        .max_wait = 0ms,
        .lease_owner_key = "loop-owner",
        .lease_ttl = 30s,
    });

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::upstream);
    REQUIRE(backend.decay_calls == 1);

    auto reacquired =
        co_await runtime->repository().acquire_memory_retention_lease(automation::AcquireMemoryRetentionLeaseRequest{
            .job_key = "memory-retention:cli",
            .owner_key = "after-error-owner",
            .acquired_at = at(61s),
            .expires_at = at(90s),
        });
    REQUIRE(reacquired.has_value());
    REQUIRE(reacquired->has_value());
    REQUIRE((*reacquired)->owner_key == "after-error-owner");
  });
}

TEST_CASE("MemoryRetentionLoop::run_once reports cancellation while waiting",
          "[unit][automation][runtime][loop][cancellation]") {
  TempWorkspace workspace{"oran-automation-runtime-loop-cancel"};
  asio::io_context io;
  asio::cancellation_signal signal;
  std::optional<core::Result<automation::MemoryRetentionLoopRunOnceResult>> result;
  std::exception_ptr failure;

  asio::steady_timer cancel_timer{io};
  cancel_timer.expires_after(10ms);
  cancel_timer.async_wait([&](const asio::error_code& ec) {
    if (!ec) {
      signal.emit(asio::cancellation_type::terminal);
    }
  });

  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<automation::MemoryRetentionLoopRunOnceResult>> {
        auto runtime = co_await automation::AutomationRuntime::open(
            io.get_executor(),
            automation::AutomationRuntimeOptions{.database_path = automation_db_path(workspace)});
        if (!runtime) {
          co_return std::unexpected(std::move(runtime).error());
        }
        auto upserted =
            co_await runtime->repository().upsert_memory_retention_job(automation::UpsertMemoryRetentionJobRequest{
                .job_key = "memory-retention:cli",
                .job = make_job(),
            });
        if (!upserted) {
          co_return std::unexpected(std::move(upserted).error());
        }

        FakeBackend backend;
        auto loop = runtime->memory_retention_loop(backend);
        co_return co_await loop.run_once(automation::MemoryRetentionLoopRunOnceRequest{
            .job_key = "memory-retention:cli",
            .now = at(59s),
            .max_wait = 2s,
        });
      },
      asio::bind_cancellation_slot(
          signal.slot(),
          [&](std::exception_ptr ep, core::Result<automation::MemoryRetentionLoopRunOnceResult> r) {
            failure = ep;
            result = std::move(r);
            cancel_timer.cancel();
            io.stop();
          }));
  io.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);

  test::run_async([&workspace](asio::io_context& check_io) -> async::Awaitable<void> {
    auto runtime = co_await automation::AutomationRuntime::open(
        check_io.get_executor(),
        automation::AutomationRuntimeOptions{.database_path = automation_db_path(workspace)});
    REQUIRE(runtime.has_value());

    auto acquired =
        co_await runtime->repository().acquire_memory_retention_lease(automation::AcquireMemoryRetentionLeaseRequest{
            .job_key = "memory-retention:cli",
            .owner_key = "after-cancel-owner",
            .acquired_at = at(59s),
            .expires_at = at(61s),
        });
    REQUIRE(acquired.has_value());
    REQUIRE(acquired->has_value());
    REQUIRE((*acquired)->owner_key == "after-cancel-owner");
  });
}

TEST_CASE("CronLoop::run_once reports cancellation while waiting",
          "[unit][automation][runtime][cron][loop][cancellation]") {
  TempWorkspace workspace{"oran-automation-runtime-cron-loop-cancel"};
  asio::io_context io;
  asio::cancellation_signal signal;
  std::optional<core::Result<automation::CronLoopRunOnceResult>> result;
  std::exception_ptr failure;

  asio::steady_timer cancel_timer{io};
  cancel_timer.expires_after(10ms);
  cancel_timer.async_wait([&](const asio::error_code& ec) {
    if (!ec) {
      signal.emit(asio::cancellation_type::terminal);
    }
  });

  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<automation::CronLoopRunOnceResult>> {
        auto runtime = co_await automation::AutomationRuntime::open(
            io.get_executor(),
            automation::AutomationRuntimeOptions{.database_path = automation_db_path(workspace)});
        if (!runtime) {
          co_return std::unexpected(std::move(runtime).error());
        }
        auto upserted = co_await runtime->repository().upsert_cron_job(automation::UpsertCronJobRequest{
            .job_key = "cron:every-minute",
            .schedule = make_cron_schedule(),
        });
        if (!upserted) {
          co_return std::unexpected(std::move(upserted).error());
        }

        auto loop = runtime->cron_loop();
        co_return co_await loop.run_once(automation::CronLoopRunOnceRequest{
            .now = at(59s),
            .max_wait = 2s,
            .job_limit = 10,
        });
      },
      asio::bind_cancellation_slot(signal.slot(),
                                   [&](std::exception_ptr ep, core::Result<automation::CronLoopRunOnceResult> r) {
                                     failure = ep;
                                     result = std::move(r);
                                     cancel_timer.cancel();
                                     io.stop();
                                   }));
  io.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);

  test::run_async([&workspace](asio::io_context& check_io) -> async::Awaitable<void> {
    auto runtime = co_await automation::AutomationRuntime::open(
        check_io.get_executor(),
        automation::AutomationRuntimeOptions{.database_path = automation_db_path(workspace)});
    REQUIRE(runtime.has_value());

    auto loaded = co_await runtime->repository().get_cron_job("cron:every-minute");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    REQUIRE_FALSE((*loaded)->state.last_fired_at.has_value());
  });
}

TEST_CASE("Cron service and loop reject invalid scan policy", "[unit][automation][runtime][cron][loop]") {
  TempWorkspace workspace{"oran-automation-runtime-cron-invalid"};
  test::run_async([&workspace](asio::io_context& io) -> async::Awaitable<void> {
    auto runtime = co_await automation::AutomationRuntime::open(
        io.get_executor(),
        automation::AutomationRuntimeOptions{.database_path = automation_db_path(workspace)});
    REQUIRE(runtime.has_value());

    auto service = runtime->cron_service();
    auto invalid_tick = co_await service.tick(automation::CronTickRequest{
        .now = at(0s),
        .job_limit = 0,
    });
    REQUIRE_FALSE(invalid_tick.has_value());
    REQUIRE(invalid_tick.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(has_field(invalid_tick.error(), "job_limit"));

    auto loop = runtime->cron_loop();
    auto invalid_wait = co_await loop.run_once(automation::CronLoopRunOnceRequest{
        .now = at(0s),
        .max_wait = -1ms,
    });
    REQUIRE_FALSE(invalid_wait.has_value());
    REQUIRE(invalid_wait.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(has_field(invalid_wait.error(), "max_wait"));

    auto invalid_limit = co_await loop.run_once(automation::CronLoopRunOnceRequest{
        .now = at(0s),
        .job_limit = 0,
    });
    REQUIRE_FALSE(invalid_limit.has_value());
    REQUIRE(invalid_limit.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(has_field(invalid_limit.error(), "job_limit"));

    auto invalid_run_wait = co_await loop.run(automation::CronLoopRunRequest{
        .now = at(0s),
        .max_total_wait = -1ms,
        .handler = [](automation::CronDueJob) -> async::Awaitable<core::Result<void>> {
          co_return core::Result<void>{};
        },
    });
    REQUIRE_FALSE(invalid_run_wait.has_value());
    REQUIRE(invalid_run_wait.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(has_field(invalid_run_wait.error(), "max_total_wait"));

    auto invalid_run_iterations = co_await loop.run(automation::CronLoopRunRequest{
        .now = at(0s),
        .max_iterations = 0,
        .handler = [](automation::CronDueJob) -> async::Awaitable<core::Result<void>> {
          co_return core::Result<void>{};
        },
    });
    REQUIRE_FALSE(invalid_run_iterations.has_value());
    REQUIRE(invalid_run_iterations.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(has_field(invalid_run_iterations.error(), "max_iterations"));

    auto invalid_run_handler = co_await loop.run(automation::CronLoopRunRequest{
        .now = at(0s),
    });
    REQUIRE_FALSE(invalid_run_handler.has_value());
    REQUIRE(invalid_run_handler.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(has_field(invalid_run_handler.error(), "handler"));

    auto invalid_run_limit = co_await loop.run(automation::CronLoopRunRequest{
        .now = at(0s),
        .job_limit = 0,
        .handler = [](automation::CronDueJob) -> async::Awaitable<core::Result<void>> {
          co_return core::Result<void>{};
        },
    });
    REQUIRE_FALSE(invalid_run_limit.has_value());
    REQUIRE(invalid_run_limit.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(has_field(invalid_run_limit.error(), "job_limit"));
  });
}

TEST_CASE("MemoryRetentionLoop::run_once rejects invalid wait and lease budgets", "[unit][automation][runtime][loop]") {
  TempWorkspace workspace{"oran-automation-runtime-loop-invalid"};
  test::run_async([&workspace](asio::io_context& io) -> async::Awaitable<void> {
    auto runtime = co_await automation::AutomationRuntime::open(
        io.get_executor(),
        automation::AutomationRuntimeOptions{.database_path = automation_db_path(workspace)});
    REQUIRE(runtime.has_value());

    FakeBackend backend;
    auto loop = runtime->memory_retention_loop(backend);
    auto result = co_await loop.run_once(automation::MemoryRetentionLoopRunOnceRequest{
        .job_key = "memory-retention:cli",
        .now = at(0s),
        .max_wait = -1ms,
    });

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(has_field(result.error(), "max_wait"));
    REQUIRE(backend.decay_calls == 0);

    auto invalid_ttl = co_await loop.run_once(automation::MemoryRetentionLoopRunOnceRequest{
        .job_key = "memory-retention:cli",
        .now = at(0s),
        .max_wait = 10ms,
        .lease_owner_key = "loop-owner",
        .lease_ttl = 0ms,
    });

    REQUIRE_FALSE(invalid_ttl.has_value());
    REQUIRE(invalid_ttl.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(has_field(invalid_ttl.error(), "lease_ttl"));
    REQUIRE(backend.decay_calls == 0);
  });
}

TEST_CASE("MemoryRetentionLoop::run rejects invalid loop policy budgets", "[unit][automation][runtime][loop]") {
  TempWorkspace workspace{"oran-automation-runtime-loop-run-invalid"};
  test::run_async([&workspace](asio::io_context& io) -> async::Awaitable<void> {
    auto runtime = co_await automation::AutomationRuntime::open(
        io.get_executor(),
        automation::AutomationRuntimeOptions{.database_path = automation_db_path(workspace)});
    REQUIRE(runtime.has_value());

    FakeBackend backend;
    auto loop = runtime->memory_retention_loop(backend);
    auto invalid_wait = co_await loop.run(automation::MemoryRetentionLoopRunRequest{
        .job_key = "memory-retention:cli",
        .now = at(0s),
        .max_total_wait = -1ms,
    });

    REQUIRE_FALSE(invalid_wait.has_value());
    REQUIRE(invalid_wait.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(has_field(invalid_wait.error(), "max_total_wait"));
    REQUIRE(backend.decay_calls == 0);

    auto invalid_iterations = co_await loop.run(automation::MemoryRetentionLoopRunRequest{
        .job_key = "memory-retention:cli",
        .now = at(0s),
        .max_total_wait = 0ms,
        .max_iterations = 0,
    });

    REQUIRE_FALSE(invalid_iterations.has_value());
    REQUIRE(invalid_iterations.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(has_field(invalid_iterations.error(), "max_iterations"));
    REQUIRE(backend.decay_calls == 0);
  });
}
