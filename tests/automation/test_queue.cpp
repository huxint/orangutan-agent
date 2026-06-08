// tests/automation/test_queue.cpp - triggered queue/backpressure coverage.

#include <algorithm>
#include <chrono>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/automation.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/hook.hpp>
#include <oran/storage.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace automation = orangutan::automation;
namespace core = orangutan::core;
namespace hook = orangutan::hook;
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

[[nodiscard]] bool contains_job(std::span<const std::string> jobs, std::string_view job_key) {
  return std::ranges::contains(jobs, job_key);
}

}  // namespace

TEST_CASE("TriggeredQueue enqueues matched triggered jobs for explicit receive",
          "[unit][automation][queue][triggered]") {
  TempDb db{"oran-automation-queue-triggered"};
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

    automation::TriggeredQueue queue{io.get_executor(),
                                     automation::TriggeredService{repo},
                                     automation::TriggeredQueueOptions{.capacity = 4}};
    auto enqueued = co_await queue.enqueue(automation::TriggeredQueueEnqueueRequest{
        .trigger_key = "webhook:ci",
        .received_at = at(120s),
        .job_limit = 10,
    });

    REQUIRE(enqueued.has_value());
    REQUIRE(enqueued->intake.matched_count == 2);
    REQUIRE(enqueued->enqueued_count == 2);
    REQUIRE(enqueued->dropped_count == 0);
    REQUIRE(enqueued->enqueued.size() == 2);
    REQUIRE(enqueued->dropped.empty());
    REQUIRE(queue.size() == 2);

    auto first = co_await queue.receive();
    REQUIRE(first.has_value());
    auto second = co_await queue.receive();
    REQUIRE(second.has_value());
    std::vector<std::string> received{first->execution.job.job_key, second->execution.job.job_key};
    REQUIRE(contains_job(received, "triggered:webhook-ci"));
    REQUIRE(contains_job(received, "triggered:webhook-ci-secondary"));
    REQUIRE(first->execution.trigger_key == "webhook:ci");
    REQUIRE(second->execution.trigger_key == "webhook:ci");
    REQUIRE(first->execution.received_at == at(120s));
    REQUIRE(second->execution.received_at == at(120s));
    REQUIRE(first->enqueued_at == at(120s));
    REQUIRE(second->enqueued_at == at(120s));

    auto runs = co_await repo.list_triggered_runs(automation::ListTriggeredRunsOptions{
        .job_key = "triggered:webhook-ci",
        .limit = 10,
    });
    REQUIRE(runs.has_value());
    REQUIRE(runs->empty());
  });
}

TEST_CASE("TriggeredQueue drops newest overflow and publishes job_dropped metadata",
          "[unit][automation][queue][triggered][hook]") {
  TempDb db{"oran-automation-queue-triggered-overflow"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());
    REQUIRE((co_await repo.upsert_triggered_job(automation::UpsertTriggeredJobRequest{
                 .job_key = "triggered:webhook-ci-a",
                 .trigger_key = "webhook:ci",
                 .agent_key = "researcher",
             }))
                .has_value());
    REQUIRE((co_await repo.upsert_triggered_job(automation::UpsertTriggeredJobRequest{
                 .job_key = "triggered:webhook-ci-b",
                 .trigger_key = "webhook:ci",
                 .agent_key = "coder",
             }))
                .has_value());

    std::vector<hook::JobDroppedPayload> dropped_payloads;
    hook::Bus bus;
    hook::InProcessSink sink{
        "triggered-queue-drop-recorder",
        [&dropped_payloads](hook::Event event, hook::PayloadPtr payload) -> async::Awaitable<core::Result<void>> {
          REQUIRE(event == hook::Event::job_dropped);
          const auto* dropped = std::get_if<hook::JobDroppedPayload>(payload.get());
          REQUIRE(dropped != nullptr);
          dropped_payloads.push_back(*dropped);
          co_return core::Result<void>{};
        }};
    bus.bind(sink, {hook::Event::job_dropped});

    automation::TriggeredQueue queue{io.get_executor(),
                                     automation::TriggeredService{repo},
                                     automation::TriggeredQueueOptions{
                                         .capacity = 1,
                                         .overflow_policy = automation::TriggeredQueueOverflowPolicy::drop_newest,
                                         .hooks =
                                             automation::TriggeredHookOptions{
                                                 .bus = &bus,
                                                 .source = "triggered-queue",
                                                 .identity = "trigger-loop",
                                             },
                                     }};
    auto result = co_await queue.enqueue(automation::TriggeredQueueEnqueueRequest{
        .trigger_key = "webhook:ci",
        .received_at = at(120s),
        .job_limit = 10,
    });

    REQUIRE(result.has_value());
    REQUIRE(result->intake.matched_count == 2);
    REQUIRE(result->enqueued_count == 1);
    REQUIRE(result->dropped_count == 1);
    REQUIRE(result->enqueued.size() == 1);
    REQUIRE(result->dropped.size() == 1);
    REQUIRE(queue.size() == 1);
    REQUIRE(result->dropped.front().reason == automation::TriggeredQueueDropReason::queue_full);
    REQUIRE(result->dropped.front().queue_capacity == 1);
    REQUIRE(result->dropped.front().queue_size == 1);
    REQUIRE(result->dropped.front().dropped_at == at(120s));
    REQUIRE(dropped_payloads.size() == 1);

    const auto& payload = dropped_payloads.front();
    REQUIRE(payload.source == "triggered-queue");
    REQUIRE(payload.who.identity == "trigger-loop");
    REQUIRE(payload.job_type == "triggered");
    REQUIRE(payload.trigger_key == "webhook:ci");
    REQUIRE(payload.reason == "queue_full");
    REQUIRE(payload.scheduled_at == at(120s));
    REQUIRE(payload.dropped_at == at(120s));
    REQUIRE(payload.queue_capacity == 1);
    REQUIRE(payload.queue_size == 1);
    REQUIRE(payload.job_key == result->dropped.front().execution.job.job_key);
    REQUIRE(payload.who.agent_key == result->dropped.front().execution.job.agent_key);

    auto received = co_await queue.receive();
    REQUIRE(received.has_value());
    REQUIRE(received->execution.job.job_key == result->enqueued.front().execution.job.job_key);

    auto dropped_runs = co_await repo.list_triggered_runs(automation::ListTriggeredRunsOptions{
        .job_key = result->dropped.front().execution.job.job_key,
        .limit = 10,
    });
    REQUIRE(dropped_runs.has_value());
    REQUIRE(dropped_runs->empty());
  });
}

TEST_CASE("TriggeredQueue drains one queued descriptor through the triggered service",
          "[unit][automation][queue][triggered]") {
  TempDb db{"oran-automation-queue-triggered-drain"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());
    REQUIRE((co_await repo.upsert_triggered_job(automation::UpsertTriggeredJobRequest{
                 .job_key = "triggered:webhook-ci-a",
                 .trigger_key = "webhook:ci",
                 .agent_key = "researcher",
             }))
                .has_value());
    REQUIRE((co_await repo.upsert_triggered_job(automation::UpsertTriggeredJobRequest{
                 .job_key = "triggered:webhook-ci-b",
                 .trigger_key = "webhook:ci",
                 .agent_key = "coder",
             }))
                .has_value());

    automation::TriggeredQueue queue{io.get_executor(),
                                     automation::TriggeredService{repo},
                                     automation::TriggeredQueueOptions{.capacity = 4}};
    auto enqueued = co_await queue.enqueue(automation::TriggeredQueueEnqueueRequest{
        .trigger_key = "webhook:ci",
        .received_at = at(120s),
        .job_limit = 10,
    });
    REQUIRE(enqueued.has_value());
    REQUIRE(enqueued->enqueued_count == 2);

    std::vector<std::string> handled;
    auto drained = co_await queue.drain_once(automation::TriggeredQueueDrainOnceRequest{
        .handler = [&handled](automation::TriggeredExecutionJob execution) -> async::Awaitable<core::Result<void>> {
          handled.push_back(execution.job.job_key);
          co_return core::Result<void>{};
        },
    });

    REQUIRE(drained.has_value());
    REQUIRE(drained->execution.completed);
    REQUIRE(drained->execution.attempt.completed);
    REQUIRE(handled.size() == 1);
    REQUIRE(drained->queued.execution.job.job_key == handled.front());
    REQUIRE(queue.size() == 1);

    auto drained_runs = co_await repo.list_triggered_runs(automation::ListTriggeredRunsOptions{
        .job_key = drained->queued.execution.job.job_key,
        .limit = 10,
    });
    REQUIRE(drained_runs.has_value());
    REQUIRE(drained_runs->size() == 1);

    const auto other_job = drained->queued.execution.job.job_key == "triggered:webhook-ci-a" ? "triggered:webhook-ci-b"
                                                                                             : "triggered:webhook-ci-a";
    auto other_runs = co_await repo.list_triggered_runs(automation::ListTriggeredRunsOptions{
        .job_key = other_job,
        .limit = 10,
    });
    REQUIRE(other_runs.has_value());
    REQUIRE(other_runs->empty());
  });
}

TEST_CASE("TriggeredQueue rejects invalid enqueue policy", "[unit][automation][queue][triggered]") {
  TempDb db{"oran-automation-queue-triggered-validation"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());
    automation::TriggeredQueue queue{io.get_executor(), automation::TriggeredService{repo}};

    auto missing_trigger = co_await queue.enqueue(automation::TriggeredQueueEnqueueRequest{
        .trigger_key = "",
        .received_at = at(120s),
        .job_limit = 10,
    });
    REQUIRE_FALSE(missing_trigger.has_value());
    REQUIRE(missing_trigger.error().kind() == core::ErrorKind::invalid_argument);

    auto bad_limit = co_await queue.enqueue(automation::TriggeredQueueEnqueueRequest{
        .trigger_key = "webhook:ci",
        .received_at = at(120s),
        .job_limit = 0,
    });
    REQUIRE_FALSE(bad_limit.has_value());
    REQUIRE(bad_limit.error().kind() == core::ErrorKind::invalid_argument);

    automation::TriggeredQueue zero_capacity_queue{
        io.get_executor(),
        automation::TriggeredService{repo},
        automation::TriggeredQueueOptions{.capacity = 0},
    };
    auto zero_capacity = co_await zero_capacity_queue.enqueue(automation::TriggeredQueueEnqueueRequest{
        .trigger_key = "webhook:ci",
        .received_at = at(120s),
        .job_limit = 1,
    });
    REQUIRE_FALSE(zero_capacity.has_value());
    REQUIRE(zero_capacity.error().kind() == core::ErrorKind::invalid_argument);

    auto bad_drain = co_await queue.drain_once(automation::TriggeredQueueDrainOnceRequest{});
    REQUIRE_FALSE(bad_drain.has_value());
    REQUIRE(bad_drain.error().kind() == core::ErrorKind::invalid_argument);
  });
}
