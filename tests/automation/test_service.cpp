// tests/automation/test_service.cpp - automation service tick coverage.

#include <algorithm>
#include <chrono>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/automation.hpp>
#include <oran/core/error.hpp>
#include <oran/hook.hpp>
#include <oran/memory/longterm.hpp>
#include <oran/storage.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace automation = orangutan::automation;
namespace core = orangutan::core;
namespace hook = orangutan::hook;
namespace memory = orangutan::memory;
namespace storage = orangutan::storage;
namespace test = orangutan::tests;

namespace {

using namespace std::chrono_literals;

class TempDb {
public:
  explicit TempDb(std::string name)
      : path_(std::filesystem::temp_directory_path() /
              (std::move(name) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
               ".db")) {}

  ~TempDb() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    std::filesystem::remove(path_.string() + "-wal", ec);
    std::filesystem::remove(path_.string() + "-shm", ec);
  }

  TempDb(const TempDb&) = delete;
  TempDb& operator=(const TempDb&) = delete;

  [[nodiscard]] std::string string() const {
    return path_.string();
  }

private:
  std::filesystem::path path_;
};

[[nodiscard]] core::Time at(std::chrono::seconds value) {
  return core::Time{core::Time::time_point{value}};
}

storage::Pool open_pool(asio::io_context& io, TempDb& db) {
  auto pool =
      storage::Pool::open(io.get_executor(),
                          storage::PoolOptions{.path = db.string(), .reader_count = 2, .statement_cache_capacity = 8});
  REQUIRE(pool.has_value());
  return std::move(*pool);
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
    if (decay_error.has_value()) {
      auto error = std::move(*decay_error);
      co_return std::unexpected(std::move(error));
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

struct CapturedJobLifecycle {
  hook::Event event{};
  hook::JobLifecyclePayload payload{};
};

}  // namespace

TEST_CASE("TriggeredService::intake matches stored jobs for a trigger key", "[unit][automation][service][triggered]") {
  TempDb db{"oran-automation-service-triggered"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());
    REQUIRE((co_await repo.upsert_triggered_job(automation::UpsertTriggeredJobRequest{
                 .job_key = "triggered:webhook-ci",
                 .trigger_key = "webhook:ci",
                 .agent_key = "researcher",
             }))
                .has_value());
    REQUIRE((co_await repo.upsert_triggered_job(automation::UpsertTriggeredJobRequest{
                 .job_key = "triggered:webhook-ci-secondary",
                 .trigger_key = "webhook:ci",
                 .agent_key = "coder",
             }))
                .has_value());
    REQUIRE((co_await repo.upsert_triggered_job(automation::UpsertTriggeredJobRequest{
                 .job_key = "triggered:file-watch",
                 .trigger_key = "file:workspace",
             }))
                .has_value());

    automation::TriggeredService service{repo};
    auto intake = co_await service.intake(automation::TriggeredIntakeRequest{
        .trigger_key = "webhook:ci",
        .received_at = at(120s),
        .job_limit = 10,
    });

    REQUIRE(intake.has_value());
    REQUIRE(intake->trigger_key == "webhook:ci");
    REQUIRE(intake->received_at == at(120s));
    REQUIRE(intake->matched_count == 2);
    REQUIRE(intake->jobs.size() == 2);
    auto first = std::ranges::find_if(intake->jobs, [](const auto& job) {
      return job.job_key == "triggered:webhook-ci" && job.agent_key == "researcher";
    });
    REQUIRE(first != intake->jobs.end());
    auto second = std::ranges::find_if(intake->jobs, [](const auto& job) {
      return job.job_key == "triggered:webhook-ci-secondary" && job.agent_key == "coder";
    });
    REQUIRE(second != intake->jobs.end());
  });
}

TEST_CASE("TriggeredService::intake rejects invalid intake policy", "[unit][automation][service][triggered]") {
  TempDb db{"oran-automation-service-triggered-validation"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());

    automation::TriggeredService service{repo};
    auto missing_trigger = co_await service.intake(automation::TriggeredIntakeRequest{
        .trigger_key = "",
        .received_at = at(120s),
        .job_limit = 10,
    });
    REQUIRE_FALSE(missing_trigger.has_value());
    REQUIRE(missing_trigger.error().kind() == core::ErrorKind::invalid_argument);

    auto bad_limit = co_await service.intake(automation::TriggeredIntakeRequest{
        .trigger_key = "webhook:ci",
        .received_at = at(120s),
        .job_limit = 0,
    });
    REQUIRE_FALSE(bad_limit.has_value());
    REQUIRE(bad_limit.error().kind() == core::ErrorKind::invalid_argument);
  });
}

TEST_CASE("TriggeredService::execute records explicit triggered handler attempts",
          "[unit][automation][service][triggered]") {
  TempDb db{"oran-automation-service-triggered-execute"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());
    REQUIRE((co_await repo.upsert_triggered_job(automation::UpsertTriggeredJobRequest{
                 .job_key = "triggered:succeeds",
                 .trigger_key = "webhook:ci",
                 .agent_key = "researcher",
             }))
                .has_value());
    REQUIRE((co_await repo.upsert_triggered_job(automation::UpsertTriggeredJobRequest{
                 .job_key = "triggered:fails",
                 .trigger_key = "webhook:ci",
                 .agent_key = "coder",
             }))
                .has_value());

    std::vector<std::string> calls;
    automation::TriggeredService service{repo};
    auto result = co_await service.execute(automation::TriggeredExecuteRequest{
        .trigger_key = "webhook:ci",
        .received_at = at(120s),
        .job_limit = 10,
        .handler = [&calls](automation::TriggeredExecutionJob execution) -> async::Awaitable<core::Result<void>> {
          calls.push_back(execution.job.job_key);
          REQUIRE(execution.trigger_key == "webhook:ci");
          REQUIRE(execution.received_at == at(120s));
          if (execution.job.job_key == "triggered:fails") {
            co_return std::unexpected(core::Error::upstream("triggered payload failed"));
          }
          co_return core::Result<void>{};
        },
    });

    REQUIRE(result.has_value());
    REQUIRE(result->intake.trigger_key == "webhook:ci");
    REQUIRE(result->intake.matched_count == 2);
    REQUIRE(result->attempted_count == 2);
    REQUIRE(result->completed_count == 1);
    REQUIRE(result->attempts.size() == 2);
    REQUIRE(calls.size() == 2);

    auto success = std::ranges::find_if(result->attempts, [](const auto& attempt) {
      return attempt.execution.job.job_key == "triggered:succeeds";
    });
    REQUIRE(success != result->attempts.end());
    REQUIRE(success->completed);
    REQUIRE(success->run.has_value());
    REQUIRE(success->run->outcome == automation::TriggeredRunOutcome::success);
    REQUIRE_FALSE(success->error.has_value());

    auto failure = std::ranges::find_if(result->attempts, [](const auto& attempt) {
      return attempt.execution.job.job_key == "triggered:fails";
    });
    REQUIRE(failure != result->attempts.end());
    REQUIRE_FALSE(failure->completed);
    REQUIRE(failure->run.has_value());
    REQUIRE(failure->run->outcome == automation::TriggeredRunOutcome::failure);
    REQUIRE(failure->run->error_message == "triggered payload failed");
    REQUIRE(failure->error.has_value());

    auto success_runs = co_await repo.list_triggered_runs(automation::ListTriggeredRunsOptions{
        .job_key = "triggered:succeeds",
        .limit = 10,
    });
    REQUIRE(success_runs.has_value());
    REQUIRE(success_runs->size() == 1);
    REQUIRE(success_runs->front().trigger_key == "webhook:ci");
    REQUIRE(success_runs->front().success);

    auto failure_runs = co_await repo.list_triggered_runs(automation::ListTriggeredRunsOptions{
        .job_key = "triggered:fails",
        .limit = 10,
    });
    REQUIRE(failure_runs.has_value());
    REQUIRE(failure_runs->size() == 1);
    REQUIRE_FALSE(failure_runs->front().success);
    REQUIRE(failure_runs->front().outcome == automation::TriggeredRunOutcome::failure);
  });
}

TEST_CASE("TriggeredService::execute records cancelled triggered handlers as aborted",
          "[unit][automation][service][triggered]") {
  TempDb db{"oran-automation-service-triggered-aborted"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());
    REQUIRE((co_await repo.upsert_triggered_job(automation::UpsertTriggeredJobRequest{
                 .job_key = "triggered:cancelled",
                 .trigger_key = "webhook:ci",
             }))
                .has_value());

    automation::TriggeredService service{repo};
    auto result = co_await service.execute(automation::TriggeredExecuteRequest{
        .trigger_key = "webhook:ci",
        .received_at = at(120s),
        .job_limit = 10,
        .handler = [](automation::TriggeredExecutionJob) -> async::Awaitable<core::Result<void>> {
          co_return std::unexpected(core::Error::cancelled());
        },
    });

    REQUIRE(result.has_value());
    REQUIRE(result->attempted_count == 1);
    REQUIRE(result->completed_count == 0);
    REQUIRE(result->attempts.size() == 1);
    REQUIRE(result->attempts.front().run.has_value());
    REQUIRE(result->attempts.front().run->outcome == automation::TriggeredRunOutcome::aborted);
    REQUIRE(result->attempts.front().error.has_value());
    REQUIRE(result->attempts.front().error->kind() == core::ErrorKind::cancelled);

    auto runs = co_await repo.list_triggered_runs(automation::ListTriggeredRunsOptions{
        .job_key = "triggered:cancelled",
        .limit = 10,
    });
    REQUIRE(runs.has_value());
    REQUIRE(runs->size() == 1);
    REQUIRE(runs->front().outcome == automation::TriggeredRunOutcome::aborted);
    REQUIRE(runs->front().error_message.has_value());
  });
}

TEST_CASE("TriggeredService::execute rejects invalid execution policy", "[unit][automation][service][triggered]") {
  TempDb db{"oran-automation-service-triggered-execute-validation"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());

    automation::TriggeredService service{repo};
    auto missing_handler = co_await service.execute(automation::TriggeredExecuteRequest{
        .trigger_key = "webhook:ci",
        .received_at = at(120s),
        .job_limit = 10,
    });
    REQUIRE_FALSE(missing_handler.has_value());
    REQUIRE(missing_handler.error().kind() == core::ErrorKind::invalid_argument);

    auto bad_limit = co_await service.execute(automation::TriggeredExecuteRequest{
        .trigger_key = "webhook:ci",
        .received_at = at(120s),
        .job_limit = 0,
        .handler = [](automation::TriggeredExecutionJob) -> async::Awaitable<core::Result<void>> {
          co_return core::Result<void>{};
        },
    });
    REQUIRE_FALSE(bad_limit.has_value());
    REQUIRE(bad_limit.error().kind() == core::ErrorKind::invalid_argument);
  });
}

TEST_CASE("CronService::execute_due advances only successful due cron jobs", "[unit][automation][service][cron]") {
  TempDb db{"oran-automation-service-cron-execute"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());
    REQUIRE((co_await repo.upsert_cron_job(automation::UpsertCronJobRequest{
                 .job_key = "cron:succeeds",
                 .schedule = make_cron_schedule(),
             }))
                .has_value());
    REQUIRE((co_await repo.upsert_cron_job(automation::UpsertCronJobRequest{
                 .job_key = "cron:fails",
                 .schedule = make_cron_schedule(),
             }))
                .has_value());

    std::vector<std::string> calls;
    automation::CronService service{repo};
    auto result = co_await service.execute_due(automation::CronExecuteRequest{
        .now = at(120s),
        .job_limit = 10,
        .handler = [&calls](automation::CronDueJob due) -> async::Awaitable<core::Result<void>> {
          calls.push_back(due.job.job_key);
          if (due.job.job_key == "cron:fails") {
            co_return std::unexpected(core::Error::upstream("cron payload failed"));
          }
          co_return core::Result<void>{};
        },
    });

    REQUIRE(result.has_value());
    REQUIRE(result->tick.checked_count == 2);
    REQUIRE(result->tick.due_jobs.size() == 2);
    REQUIRE(result->attempted_count == 2);
    REQUIRE(result->advanced_count == 1);
    REQUIRE(result->attempts.size() == 2);
    REQUIRE(calls.size() == 2);

    const auto success = std::ranges::find_if(result->attempts, [](const auto& attempt) {
      return attempt.due.job.job_key == "cron:succeeds";
    });
    REQUIRE(success != result->attempts.end());
    REQUIRE(success->advanced);
    REQUIRE_FALSE(success->error.has_value());
    REQUIRE(success->run.has_value());
    REQUIRE(success->run->job_key == "cron:succeeds");
    REQUIRE(success->run->fired_at == at(60s));
    REQUIRE(success->run->success);
    REQUIRE(success->run->outcome == automation::CronRunOutcome::success);
    REQUIRE_FALSE(success->run->error_message.has_value());
    REQUIRE(success->marked_job.has_value());
    REQUIRE(success->marked_job->state.last_fired_at == at(60s));

    const auto failure = std::ranges::find_if(result->attempts, [](const auto& attempt) {
      return attempt.due.job.job_key == "cron:fails";
    });
    REQUIRE(failure != result->attempts.end());
    REQUIRE_FALSE(failure->advanced);
    REQUIRE(failure->error.has_value());
    REQUIRE(failure->error->kind() == core::ErrorKind::upstream);
    REQUIRE(failure->run.has_value());
    REQUIRE(failure->run->job_key == "cron:fails");
    REQUIRE_FALSE(failure->run->success);
    REQUIRE(failure->run->outcome == automation::CronRunOutcome::failure);
    REQUIRE(failure->run->error_message == "cron payload failed");
    REQUIRE_FALSE(failure->marked_job.has_value());

    auto advanced = co_await repo.get_cron_job("cron:succeeds");
    REQUIRE(advanced.has_value());
    REQUIRE(advanced->has_value());
    REQUIRE((*advanced)->state.last_fired_at == at(60s));

    auto unchanged = co_await repo.get_cron_job("cron:fails");
    REQUIRE(unchanged.has_value());
    REQUIRE(unchanged->has_value());
    REQUIRE_FALSE((*unchanged)->state.last_fired_at.has_value());

    auto success_runs = co_await repo.list_cron_runs(automation::ListCronRunsOptions{
        .job_key = "cron:succeeds",
        .limit = 10,
    });
    REQUIRE(success_runs.has_value());
    REQUIRE(success_runs->size() == 1);
    REQUIRE((*success_runs)[0].success);
    REQUIRE((*success_runs)[0].outcome == automation::CronRunOutcome::success);

    auto failure_runs = co_await repo.list_cron_runs(automation::ListCronRunsOptions{
        .job_key = "cron:fails",
        .limit = 10,
    });
    REQUIRE(failure_runs.has_value());
    REQUIRE(failure_runs->size() == 1);
    REQUIRE_FALSE((*failure_runs)[0].success);
    REQUIRE((*failure_runs)[0].outcome == automation::CronRunOutcome::failure);
    REQUIRE((*failure_runs)[0].error_message == "cron payload failed");
  });
}

TEST_CASE("CronService::execute_due records cancelled cron handlers as aborted", "[unit][automation][service][cron]") {
  TempDb db{"oran-automation-service-cron-cancelled"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());
    REQUIRE((co_await repo.upsert_cron_job(automation::UpsertCronJobRequest{
                 .job_key = "cron:cancelled",
                 .schedule = make_cron_schedule(),
             }))
                .has_value());

    automation::CronService service{repo};
    auto result = co_await service.execute_due(automation::CronExecuteRequest{
        .now = at(120s),
        .job_limit = 10,
        .handler = [](automation::CronDueJob) -> async::Awaitable<core::Result<void>> {
          co_return std::unexpected(core::Error::cancelled());
        },
    });

    REQUIRE(result.has_value());
    REQUIRE(result->attempted_count == 1);
    REQUIRE(result->advanced_count == 0);
    REQUIRE(result->attempts.size() == 1);
    const auto& attempt = result->attempts[0];
    REQUIRE(attempt.due.job.job_key == "cron:cancelled");
    REQUIRE_FALSE(attempt.advanced);
    REQUIRE(attempt.error.has_value());
    REQUIRE(attempt.error->kind() == core::ErrorKind::cancelled);
    REQUIRE(attempt.run.has_value());
    REQUIRE(attempt.run->job_key == "cron:cancelled");
    REQUIRE_FALSE(attempt.run->success);
    REQUIRE(attempt.run->outcome == automation::CronRunOutcome::aborted);
    REQUIRE(attempt.run->error_message == "cancelled");
    REQUIRE_FALSE(attempt.marked_job.has_value());

    auto loaded = co_await repo.get_cron_job("cron:cancelled");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    REQUIRE_FALSE((*loaded)->state.last_fired_at.has_value());

    auto runs = co_await repo.list_cron_runs(automation::ListCronRunsOptions{
        .job_key = "cron:cancelled",
        .limit = 10,
    });
    REQUIRE(runs.has_value());
    REQUIRE(runs->size() == 1);
    REQUIRE_FALSE((*runs)[0].success);
    REQUIRE((*runs)[0].outcome == automation::CronRunOutcome::aborted);
    REQUIRE((*runs)[0].error_message == "cancelled");
  });
}

TEST_CASE("CronService::execute_due leases cron handlers and reports active conflicts",
          "[unit][automation][service][cron][lease]") {
  TempDb db{"oran-automation-service-cron-lease"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());
    REQUIRE((co_await repo.upsert_cron_job(automation::UpsertCronJobRequest{
                 .job_key = "cron:leased",
                 .schedule = make_cron_schedule(),
             }))
                .has_value());

    int handler_calls{};
    automation::CronService service{repo};
    auto result = co_await service.execute_due(automation::CronExecuteRequest{
        .now = at(120s),
        .job_limit = 10,
        .handler = [&handler_calls](automation::CronDueJob) -> async::Awaitable<core::Result<void>> {
          ++handler_calls;
          co_return core::Result<void>{};
        },
        .lease_owner_key = "owner-a",
        .lease_ttl = 60s,
    });

    REQUIRE(result.has_value());
    REQUIRE(result->attempted_count == 1);
    REQUIRE(result->advanced_count == 1);
    REQUIRE(handler_calls == 1);

    auto reacquired = co_await repo.acquire_cron_lease(automation::AcquireCronLeaseRequest{
        .job_key = "cron:leased",
        .owner_key = "owner-b",
        .acquired_at = at(121s),
        .expires_at = at(240s),
    });
    REQUIRE(reacquired.has_value());
    REQUIRE(reacquired->has_value());
    REQUIRE((*reacquired)->owner_key == "owner-b");

    auto blocked = co_await service.execute_due(automation::CronExecuteRequest{
        .now = at(180s),
        .job_limit = 10,
        .handler = [&handler_calls](automation::CronDueJob) -> async::Awaitable<core::Result<void>> {
          ++handler_calls;
          co_return core::Result<void>{};
        },
        .lease_owner_key = "owner-a",
        .lease_ttl = 60s,
    });
    REQUIRE_FALSE(blocked.has_value());
    REQUIRE(blocked.error().kind() == core::ErrorKind::conflict);
    REQUIRE(handler_calls == 1);

    auto loaded = co_await repo.get_cron_job("cron:leased");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    REQUIRE((*loaded)->state.last_fired_at == at(60s));
  });
}

TEST_CASE("CronService::execute_due blocks active cron agent leases before handlers",
          "[unit][automation][service][cron][lease]") {
  TempDb db{"oran-automation-service-cron-agent-lease"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());
    REQUIRE((co_await repo.upsert_cron_job(automation::UpsertCronJobRequest{
                 .job_key = "cron:research",
                 .agent_key = "researcher",
                 .schedule = make_cron_schedule(),
             }))
                .has_value());
    auto held = co_await repo.acquire_cron_agent_lease(automation::AcquireCronAgentLeaseRequest{
        .agent_key = "researcher",
        .owner_key = "owner-b",
        .acquired_at = at(100s),
        .expires_at = at(200s),
    });
    REQUIRE(held.has_value());
    REQUIRE(held->has_value());

    int handler_calls{};
    automation::CronService service{repo};
    auto blocked = co_await service.execute_due(automation::CronExecuteRequest{
        .now = at(120s),
        .job_limit = 10,
        .handler = [&handler_calls](automation::CronDueJob) -> async::Awaitable<core::Result<void>> {
          ++handler_calls;
          co_return core::Result<void>{};
        },
        .lease_owner_key = "owner-a",
        .lease_ttl = 60s,
    });
    REQUIRE_FALSE(blocked.has_value());
    REQUIRE(blocked.error().kind() == core::ErrorKind::conflict);
    REQUIRE(handler_calls == 0);

    auto job_reacquired = co_await repo.acquire_cron_lease(automation::AcquireCronLeaseRequest{
        .job_key = "cron:research",
        .owner_key = "owner-c",
        .acquired_at = at(121s),
        .expires_at = at(240s),
    });
    REQUIRE(job_reacquired.has_value());
    REQUIRE(job_reacquired->has_value());
  });
}

TEST_CASE("CronService::execute_due skips handlers before cron jobs are due", "[unit][automation][service][cron]") {
  TempDb db{"oran-automation-service-cron-not-due"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());
    auto schedule = make_cron_schedule("*/5 * * * *");
    schedule.first_fire_at = at(300s);
    REQUIRE((co_await repo.upsert_cron_job(automation::UpsertCronJobRequest{
                 .job_key = "cron:five-minute",
                 .schedule = schedule,
             }))
                .has_value());

    int handler_calls{};
    automation::CronService service{repo};
    auto result = co_await service.execute_due(automation::CronExecuteRequest{
        .now = at(0s),
        .job_limit = 10,
        .handler = [&handler_calls](automation::CronDueJob) -> async::Awaitable<core::Result<void>> {
          ++handler_calls;
          co_return core::Result<void>{};
        },
    });

    REQUIRE(result.has_value());
    REQUIRE(result->tick.checked_count == 1);
    REQUIRE(result->tick.due_jobs.empty());
    REQUIRE(result->tick.next_fire_at == at(300s));
    REQUIRE(result->attempted_count == 0);
    REQUIRE(result->advanced_count == 0);
    REQUIRE(result->attempts.empty());
    REQUIRE(handler_calls == 0);

    auto loaded = co_await repo.get_cron_job("cron:five-minute");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    REQUIRE_FALSE((*loaded)->state.last_fired_at.has_value());

    auto runs = co_await repo.list_cron_runs(automation::ListCronRunsOptions{
        .job_key = "cron:five-minute",
        .limit = 10,
    });
    REQUIRE(runs.has_value());
    REQUIRE(runs->empty());
  });
}

TEST_CASE("CronService::execute_due publishes lifecycle metadata for handler success",
          "[unit][automation][service][cron][hook]") {
  TempDb db{"oran-automation-service-cron-lifecycle-success"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());
    REQUIRE((co_await repo.upsert_cron_job(automation::UpsertCronJobRequest{
                 .job_key = "cron:succeeds",
                 .agent_key = "researcher",
                 .schedule = make_cron_schedule(),
                 .state = automation::PeriodicJobState{.last_fired_at = at(60s)},
             }))
                .has_value());

    std::vector<CapturedJobLifecycle> lifecycle;
    hook::Bus bus;
    hook::InProcessSink sink{
        "cron-lifecycle-success-recorder",
        [&lifecycle](hook::Event event, hook::PayloadPtr payload) -> async::Awaitable<core::Result<void>> {
          const auto* job = std::get_if<hook::JobLifecyclePayload>(payload.get());
          REQUIRE(job != nullptr);
          lifecycle.push_back(CapturedJobLifecycle{.event = event, .payload = *job});
          co_return core::Result<void>{};
        }};
    bus.bind(sink, {hook::Event::job_started, hook::Event::job_finished, hook::Event::job_failed});
    automation::CronService service{repo,
                                    automation::CronServiceOptions{
                                        .hooks =
                                            automation::CronHookOptions{
                                                .bus = &bus,
                                                .source = "cron",
                                                .agent_key = "automation-service",
                                                .identity = "cron-loop",
                                            },
                                    }};

    auto result = co_await service.execute_due(automation::CronExecuteRequest{
        .now = at(120s),
        .job_limit = 10,
        .handler = [](automation::CronDueJob) -> async::Awaitable<core::Result<void>> {
          co_return core::Result<void>{};
        },
    });

    REQUIRE(result.has_value());
    REQUIRE(result->advanced_count == 1);
    REQUIRE(lifecycle.size() == 2);
    REQUIRE(lifecycle[0].event == hook::Event::job_started);
    REQUIRE(lifecycle[1].event == hook::Event::job_finished);

    const auto& started = lifecycle[0].payload;
    REQUIRE(started.who.scope_key.empty());
    REQUIRE(started.who.agent_key == "researcher");
    REQUIRE(started.who.identity == "cron-loop");
    REQUIRE(started.source == "cron");
    REQUIRE(started.job_key == "cron:succeeds");
    REQUIRE(started.job_type == "cron");
    REQUIRE(started.scope_key.empty());
    REQUIRE(started.scheduled_at == at(120s));
    REQUIRE(started.started_at == at(120s));
    REQUIRE_FALSE(started.finished_at.has_value());
    REQUIRE_FALSE(started.duration.has_value());
    REQUIRE_FALSE(started.succeeded);
    REQUIRE_FALSE(started.shadowed_count.has_value());
    REQUIRE(started.error_kind.empty());
    REQUIRE(started.error_message.empty());

    const auto& finished = lifecycle[1].payload;
    REQUIRE(finished.job_key == "cron:succeeds");
    REQUIRE(finished.job_type == "cron");
    REQUIRE(finished.scheduled_at == at(120s));
    REQUIRE(finished.started_at == at(120s));
    REQUIRE(finished.finished_at == at(120s));
    REQUIRE(finished.duration == 0s);
    REQUIRE(finished.succeeded);
    REQUIRE_FALSE(finished.shadowed_count.has_value());
    REQUIRE(finished.error_kind.empty());
    REQUIRE(finished.error_message.empty());
  });
}

TEST_CASE("CronService::execute_due publishes lifecycle metadata for handler failure",
          "[unit][automation][service][cron][hook]") {
  TempDb db{"oran-automation-service-cron-lifecycle-failure"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());
    REQUIRE((co_await repo.upsert_cron_job(automation::UpsertCronJobRequest{
                 .job_key = "cron:fails",
                 .schedule = make_cron_schedule(),
             }))
                .has_value());

    std::vector<CapturedJobLifecycle> lifecycle;
    hook::Bus bus;
    hook::InProcessSink sink{
        "cron-lifecycle-failure-recorder",
        [&lifecycle](hook::Event event, hook::PayloadPtr payload) -> async::Awaitable<core::Result<void>> {
          const auto* job = std::get_if<hook::JobLifecyclePayload>(payload.get());
          REQUIRE(job != nullptr);
          lifecycle.push_back(CapturedJobLifecycle{.event = event, .payload = *job});
          co_return core::Result<void>{};
        }};
    bus.bind(sink, {hook::Event::job_started, hook::Event::job_finished, hook::Event::job_failed});
    automation::CronService service{repo,
                                    automation::CronServiceOptions{
                                        .hooks = automation::CronHookOptions{.bus = &bus},
                                    }};

    auto result = co_await service.execute_due(automation::CronExecuteRequest{
        .now = at(60s),
        .job_limit = 10,
        .handler = [](automation::CronDueJob) -> async::Awaitable<core::Result<void>> {
          co_return std::unexpected(core::Error::upstream("cron payload failed"));
        },
    });

    REQUIRE(result.has_value());
    REQUIRE(result->attempted_count == 1);
    REQUIRE(result->advanced_count == 0);
    REQUIRE(result->attempts.size() == 1);
    REQUIRE(result->attempts[0].error.has_value());
    REQUIRE(lifecycle.size() == 2);
    REQUIRE(lifecycle[0].event == hook::Event::job_started);
    REQUIRE(lifecycle[1].event == hook::Event::job_failed);
    REQUIRE(lifecycle[0].payload.job_key == "cron:fails");
    REQUIRE(lifecycle[0].payload.who.agent_key == "automation");
    REQUIRE(lifecycle[0].payload.job_type == "cron");
    REQUIRE(lifecycle[0].payload.scheduled_at == at(60s));
    REQUIRE_FALSE(lifecycle[0].payload.finished_at.has_value());
    REQUIRE(lifecycle[1].payload.job_key == "cron:fails");
    REQUIRE(lifecycle[1].payload.job_type == "cron");
    REQUIRE(lifecycle[1].payload.source == "cron");
    REQUIRE(lifecycle[1].payload.scheduled_at == at(60s));
    REQUIRE(lifecycle[1].payload.finished_at == at(60s));
    REQUIRE(lifecycle[1].payload.duration == 0s);
    REQUIRE_FALSE(lifecycle[1].payload.succeeded);
    REQUIRE_FALSE(lifecycle[1].payload.shadowed_count.has_value());
    REQUIRE(lifecycle[1].payload.error_kind == "upstream");
    REQUIRE(lifecycle[1].payload.error_message == "cron payload failed");

    auto loaded = co_await repo.get_cron_job("cron:fails");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    REQUIRE_FALSE((*loaded)->state.last_fired_at.has_value());
  });
}

TEST_CASE("CronService::execute_due rejects invalid execution policy", "[unit][automation][service][cron]") {
  TempDb db{"oran-automation-service-cron-validation"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());
    automation::CronService service{repo};

    auto missing_handler = co_await service.execute_due(automation::CronExecuteRequest{
        .now = at(60s),
    });
    REQUIRE_FALSE(missing_handler.has_value());
    REQUIRE(missing_handler.error().kind() == core::ErrorKind::invalid_argument);

    auto zero_limit = co_await service.execute_due(automation::CronExecuteRequest{
        .now = at(60s),
        .job_limit = 0,
        .handler = [](automation::CronDueJob) -> async::Awaitable<core::Result<void>> {
          co_return core::Result<void>{};
        },
    });
    REQUIRE_FALSE(zero_limit.has_value());
    REQUIRE(zero_limit.error().kind() == core::ErrorKind::invalid_argument);

    auto invalid_lease_ttl = co_await service.execute_due(automation::CronExecuteRequest{
        .now = at(60s),
        .handler = [](automation::CronDueJob) -> async::Awaitable<core::Result<void>> {
          co_return core::Result<void>{};
        },
        .lease_owner_key = "owner-a",
        .lease_ttl = 0s,
    });
    REQUIRE_FALSE(invalid_lease_ttl.has_value());
    REQUIRE(invalid_lease_ttl.error().kind() == core::ErrorKind::invalid_argument);
  });
}

TEST_CASE("MemoryRetentionService::tick skips a stored job before it is due", "[unit][automation][service]") {
  TempDb db{"oran-automation-service-not-due"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());
    REQUIRE((co_await repo.upsert_memory_retention_job(automation::UpsertMemoryRetentionJobRequest{
                 .job_key = "memory-retention:cli",
                 .job = make_job(),
             }))
                .has_value());
    FakeBackend backend;
    std::vector<CapturedJobLifecycle> lifecycle;
    hook::Bus bus;
    hook::InProcessSink sink{
        "not-due-job-lifecycle-recorder",
        [&lifecycle](hook::Event event, hook::PayloadPtr payload) -> async::Awaitable<core::Result<void>> {
          const auto* job = std::get_if<hook::JobLifecyclePayload>(payload.get());
          REQUIRE(job != nullptr);
          lifecycle.push_back(CapturedJobLifecycle{.event = event, .payload = *job});
          co_return core::Result<void>{};
        }};
    bus.bind(sink, {hook::Event::job_started, hook::Event::job_finished, hook::Event::job_failed});
    automation::MemoryRetentionService service{repo,
                                               backend,
                                               automation::MemoryRetentionServiceOptions{
                                                   .hooks = automation::MemoryRetentionHookOptions{.bus = &bus},
                                               }};

    auto result = co_await service.tick(automation::MemoryRetentionTickRequest{
        .job_key = "memory-retention:cli",
        .now = at(59s),
    });

    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->ran);
    REQUIRE_FALSE(result->schedule.due);
    REQUIRE(result->schedule.next_fire_at == at(60s));
    REQUIRE(result->job.has_value());
    REQUIRE_FALSE(result->job->state.last_fired_at.has_value());
    REQUIRE(backend.decay_calls == 0);
    REQUIRE(lifecycle.empty());

    auto runs = co_await repo.list_memory_retention_runs(automation::ListMemoryRetentionRunsOptions{
        .job_key = "memory-retention:cli",
        .limit = 10,
    });
    REQUIRE(runs.has_value());
    REQUIRE(runs->empty());
  });
}

TEST_CASE("MemoryRetentionService::tick publishes job lifecycle metadata for due success",
          "[unit][automation][service][hook]") {
  TempDb db{"oran-automation-service-job-lifecycle-success"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());
    REQUIRE((co_await repo.upsert_memory_retention_job(automation::UpsertMemoryRetentionJobRequest{
                 .job_key = "memory-retention:cli",
                 .job = make_job(),
                 .state = automation::PeriodicJobState{.last_fired_at = at(60s)},
             }))
                .has_value());
    FakeBackend backend;
    backend.decay_result.shadowed_records = {make_record("rec-1"), make_record("rec-2")};
    std::vector<CapturedJobLifecycle> lifecycle;
    hook::Bus bus;
    hook::InProcessSink sink{
        "job-lifecycle-recorder",
        [&lifecycle](hook::Event event, hook::PayloadPtr payload) -> async::Awaitable<core::Result<void>> {
          const auto* job = std::get_if<hook::JobLifecyclePayload>(payload.get());
          REQUIRE(job != nullptr);
          lifecycle.push_back(CapturedJobLifecycle{.event = event, .payload = *job});
          co_return core::Result<void>{};
        }};
    bus.bind(sink, {hook::Event::job_started, hook::Event::job_finished, hook::Event::job_failed});
    automation::MemoryRetentionService service{repo,
                                               backend,
                                               automation::MemoryRetentionServiceOptions{
                                                   .hooks =
                                                       automation::MemoryRetentionHookOptions{
                                                           .bus = &bus,
                                                           .source = "periodic",
                                                           .agent_key = "automation-service",
                                                           .identity = "retention-loop",
                                                       },
                                               }};

    auto result = co_await service.tick(automation::MemoryRetentionTickRequest{
        .job_key = "memory-retention:cli",
        .now = at(60s + 6h + 30s),
    });

    REQUIRE(result.has_value());
    REQUIRE(result->ran);
    REQUIRE(lifecycle.size() == 2);
    REQUIRE(lifecycle[0].event == hook::Event::job_started);
    REQUIRE(lifecycle[1].event == hook::Event::job_finished);

    const auto& started = lifecycle[0].payload;
    REQUIRE(started.who.scope_key == "cli");
    REQUIRE(started.who.agent_key == "automation-service");
    REQUIRE(started.who.identity == "retention-loop");
    REQUIRE(started.source == "periodic");
    REQUIRE(started.job_key == "memory-retention:cli");
    REQUIRE(started.job_type == "memory_retention");
    REQUIRE(started.scope_key == "cli");
    REQUIRE(started.scheduled_at == at(60s + 6h));
    REQUIRE(started.started_at == at(60s + 6h + 30s));
    REQUIRE_FALSE(started.finished_at.has_value());
    REQUIRE_FALSE(started.duration.has_value());
    REQUIRE_FALSE(started.succeeded);
    REQUIRE_FALSE(started.shadowed_count.has_value());
    REQUIRE(started.error_kind.empty());
    REQUIRE(started.error_message.empty());

    const auto& finished = lifecycle[1].payload;
    REQUIRE(finished.job_key == "memory-retention:cli");
    REQUIRE(finished.job_type == "memory_retention");
    REQUIRE(finished.scheduled_at == at(60s + 6h));
    REQUIRE(finished.started_at == at(60s + 6h + 30s));
    REQUIRE(finished.finished_at == at(60s + 6h + 30s));
    REQUIRE(finished.duration == 0s);
    REQUIRE(finished.succeeded);
    REQUIRE(finished.shadowed_count == 2);
    REQUIRE(finished.error_kind.empty());
    REQUIRE(finished.error_message.empty());
  });
}

TEST_CASE("MemoryRetentionService::tick runs due retention and advances last-fired", "[unit][automation][service]") {
  TempDb db{"oran-automation-service-due"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());
    REQUIRE((co_await repo.upsert_memory_retention_job(automation::UpsertMemoryRetentionJobRequest{
                 .job_key = "memory-retention:cli",
                 .job = make_job(),
                 .state = automation::PeriodicJobState{.last_fired_at = at(60s)},
             }))
                .has_value());
    FakeBackend backend;
    backend.decay_result.shadowed_records = {make_record("rec-1"), make_record("rec-2")};
    automation::MemoryRetentionService service{repo, backend};

    auto result = co_await service.tick(automation::MemoryRetentionTickRequest{
        .job_key = "memory-retention:cli",
        .now = at(60s + 6h + 30s),
    });

    REQUIRE(result.has_value());
    REQUIRE(result->ran);
    REQUIRE(result->shadowed_count == 2);
    REQUIRE(result->schedule.due);
    REQUIRE(result->schedule.next_fire_at == at(60s + 6h));
    REQUIRE(result->schedule.overdue_by == 30s);
    REQUIRE(backend.decay_calls == 1);
    REQUIRE(backend.last_decay == memory::longterm::DecayRequest{
                                      .scope_key = "cli",
                                      .unused_before = at(60s + 6h + 30s - std::chrono::days{3}),
                                      .importance_floor = 0.5,
                                      .limit = 7,
                                      .decay_at = at(60s + 6h + 30s),
                                  });
    REQUIRE(result->run.has_value());
    REQUIRE(result->run->success);
    REQUIRE(result->run->shadowed_count == 2);
    REQUIRE(result->run->fired_at == at(60s + 6h));
    REQUIRE(result->run->finished_at == at(60s + 6h + 30s));
    REQUIRE(result->job.has_value());
    REQUIRE(result->job->state.last_fired_at == at(60s + 6h));

    auto loaded = co_await repo.get_memory_retention_job("memory-retention:cli");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    REQUIRE((*loaded)->state.last_fired_at == at(60s + 6h));
  });
}

TEST_CASE("MemoryRetentionService::tick publishes memory decay metadata after successful due retention",
          "[unit][automation][service][hook]") {
  TempDb db{"oran-automation-service-hook"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());
    REQUIRE((co_await repo.upsert_memory_retention_job(automation::UpsertMemoryRetentionJobRequest{
                 .job_key = "memory-retention:cli",
                 .job = make_job(),
             }))
                .has_value());
    FakeBackend backend;
    backend.decay_result.shadowed_records = {make_record("rec-1")};

    std::vector<hook::MemoryDecayPayload> captured;
    hook::Bus bus;
    hook::InProcessSink sink{
        "retention-decay-recorder",
        [&captured](hook::Event event, hook::PayloadPtr payload) -> async::Awaitable<core::Result<void>> {
          REQUIRE(event == hook::Event::memory_decay);
          const auto* decay = std::get_if<hook::MemoryDecayPayload>(payload.get());
          REQUIRE(decay != nullptr);
          captured.push_back(*decay);
          co_return core::Result<void>{};
        }};
    bus.bind(sink, {hook::Event::memory_decay});

    automation::MemoryRetentionService service{repo,
                                               backend,
                                               automation::MemoryRetentionServiceOptions{
                                                   .hooks =
                                                       automation::MemoryRetentionHookOptions{
                                                           .bus = &bus,
                                                           .source = "periodic",
                                                           .agent_key = "automation-service",
                                                           .identity = "retention-loop",
                                                       },
                                               }};

    auto result = co_await service.tick(automation::MemoryRetentionTickRequest{
        .job_key = "memory-retention:cli",
        .now = at(60s),
    });

    REQUIRE(result.has_value());
    REQUIRE(result->ran);
    REQUIRE(result->hook_publish.has_value());
    REQUIRE(result->hook_publish->sink_count == 1);
    REQUIRE(result->hook_publish->failure_count == 0);
    REQUIRE(captured.size() == 1);
    REQUIRE(captured[0].who.scope_key == "cli");
    REQUIRE(captured[0].who.agent_key == "automation-service");
    REQUIRE(captured[0].who.identity == "retention-loop");
    REQUIRE(captured[0].source == "periodic");
    REQUIRE(captured[0].scope_key == "cli");
    REQUIRE(captured[0].unused_before == at(60s - std::chrono::days{3}));
    REQUIRE(captured[0].importance_floor == 0.5);
    REQUIRE(captured[0].limit == 7);
    REQUIRE(captured[0].decay_at == at(60s));
    REQUIRE(captured[0].shadowed_count == 1);
    REQUIRE(captured[0].started_at == at(60s));
    REQUIRE(captured[0].finished_at == at(60s));
    REQUIRE(captured[0].duration == 0s);
  });
}

TEST_CASE("MemoryRetentionService::tick reports advisory hook failures without failing the tick",
          "[unit][automation][service][hook]") {
  TempDb db{"oran-automation-service-hook-failure"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());
    REQUIRE((co_await repo.upsert_memory_retention_job(automation::UpsertMemoryRetentionJobRequest{
                 .job_key = "memory-retention:cli",
                 .job = make_job(),
             }))
                .has_value());
    FakeBackend backend;
    hook::Bus bus;
    hook::InProcessSink sink{"failing-decay-recorder",
                             [](hook::Event, hook::PayloadPtr) -> async::Awaitable<core::Result<void>> {
                               co_return std::unexpected(core::Error::internal("sink failed"));
                             }};
    bus.bind(sink, {hook::Event::memory_decay});
    automation::MemoryRetentionService service{repo,
                                               backend,
                                               automation::MemoryRetentionServiceOptions{
                                                   .hooks = automation::MemoryRetentionHookOptions{.bus = &bus},
                                               }};

    auto result = co_await service.tick(automation::MemoryRetentionTickRequest{
        .job_key = "memory-retention:cli",
        .now = at(60s),
    });

    REQUIRE(result.has_value());
    REQUIRE(result->ran);
    REQUIRE(result->hook_publish.has_value());
    REQUIRE(result->hook_publish->sink_count == 1);
    REQUIRE(result->hook_publish->failure_count == 1);
    REQUIRE(result->job.has_value());
    REQUIRE(result->job->state.last_fired_at == at(60s));
    REQUIRE(result->run.has_value());
    REQUIRE(result->run->success);
  });
}

TEST_CASE("MemoryRetentionService::tick records backend failures without advancing state",
          "[unit][automation][service]") {
  TempDb db{"oran-automation-service-failure"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());
    REQUIRE((co_await repo.upsert_memory_retention_job(automation::UpsertMemoryRetentionJobRequest{
                 .job_key = "memory-retention:cli",
                 .job = make_job(),
             }))
                .has_value());
    FakeBackend backend;
    backend.decay_error = core::Error::upstream("backend unavailable");
    std::vector<CapturedJobLifecycle> lifecycle;
    hook::Bus bus;
    hook::InProcessSink sink{
        "failed-job-lifecycle-recorder",
        [&lifecycle](hook::Event event, hook::PayloadPtr payload) -> async::Awaitable<core::Result<void>> {
          const auto* job = std::get_if<hook::JobLifecyclePayload>(payload.get());
          REQUIRE(job != nullptr);
          lifecycle.push_back(CapturedJobLifecycle{.event = event, .payload = *job});
          co_return core::Result<void>{};
        }};
    bus.bind(sink, {hook::Event::job_started, hook::Event::job_finished, hook::Event::job_failed});
    automation::MemoryRetentionService service{repo,
                                               backend,
                                               automation::MemoryRetentionServiceOptions{
                                                   .hooks = automation::MemoryRetentionHookOptions{.bus = &bus},
                                               }};

    auto result = co_await service.tick(automation::MemoryRetentionTickRequest{
        .job_key = "memory-retention:cli",
        .now = at(60s),
    });

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::upstream);
    REQUIRE(backend.decay_calls == 1);
    REQUIRE(lifecycle.size() == 2);
    REQUIRE(lifecycle[0].event == hook::Event::job_started);
    REQUIRE(lifecycle[1].event == hook::Event::job_failed);
    REQUIRE(lifecycle[0].payload.job_key == "memory-retention:cli");
    REQUIRE(lifecycle[0].payload.scheduled_at == at(60s));
    REQUIRE_FALSE(lifecycle[0].payload.finished_at.has_value());
    REQUIRE(lifecycle[1].payload.job_key == "memory-retention:cli");
    REQUIRE(lifecycle[1].payload.scheduled_at == at(60s));
    REQUIRE(lifecycle[1].payload.finished_at == at(60s));
    REQUIRE(lifecycle[1].payload.duration == 0s);
    REQUIRE_FALSE(lifecycle[1].payload.succeeded);
    REQUIRE_FALSE(lifecycle[1].payload.shadowed_count.has_value());
    REQUIRE(lifecycle[1].payload.error_kind == "upstream");
    REQUIRE(lifecycle[1].payload.error_message == "backend unavailable");

    auto loaded = co_await repo.get_memory_retention_job("memory-retention:cli");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    REQUIRE_FALSE((*loaded)->state.last_fired_at.has_value());

    auto runs = co_await repo.list_memory_retention_runs(automation::ListMemoryRetentionRunsOptions{
        .job_key = "memory-retention:cli",
        .limit = 10,
    });
    REQUIRE(runs.has_value());
    REQUIRE(runs->size() == 1);
    REQUIRE_FALSE((*runs)[0].success);
    REQUIRE((*runs)[0].shadowed_count == 0);
    REQUIRE((*runs)[0].error_message == "backend unavailable");
    REQUIRE((*runs)[0].fired_at == at(60s));
  });
}

TEST_CASE("MemoryRetentionService::tick rejects invalid and missing jobs", "[unit][automation][service]") {
  TempDb db{"oran-automation-service-validation"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());
    FakeBackend backend;
    automation::MemoryRetentionService service{repo, backend};

    auto invalid = co_await service.tick(automation::MemoryRetentionTickRequest{
        .job_key = "",
        .now = at(60s),
    });
    REQUIRE_FALSE(invalid.has_value());
    REQUIRE(invalid.error().kind() == core::ErrorKind::invalid_argument);

    auto missing = co_await service.tick(automation::MemoryRetentionTickRequest{
        .job_key = "missing",
        .now = at(60s),
    });
    REQUIRE_FALSE(missing.has_value());
    REQUIRE(missing.error().kind() == core::ErrorKind::not_found);
    REQUIRE(backend.decay_calls == 0);
  });
}
