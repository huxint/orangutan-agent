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
                 .agent_prompt = "Handle triggered automation job.",
             }))
                .has_value());
    REQUIRE((co_await repo.upsert_triggered_job(automation::UpsertTriggeredJobRequest{
                 .job_key = "triggered:webhook-ci-secondary",
                 .trigger_key = "webhook:ci",
                 .agent_key = "coder",
                 .agent_prompt = "Handle triggered automation job.",
             }))
                .has_value());

    automation::TriggeredQueue queue{io.get_executor(),
                                     automation::TriggeredService{repo},
                                     automation::TriggeredQueueOptions{.capacity = 4}};
    auto enqueued = co_await queue.enqueue(automation::TriggeredQueueEnqueueRequest{
        .trigger_key = "webhook:ci",
        .trigger_payload = std::string{R"({"status":"failed"})"},
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
    REQUIRE(first->execution.trigger_payload == R"({"status":"failed"})");
    REQUIRE(second->execution.trigger_payload == R"({"status":"failed"})");
    REQUIRE(first->execution.job.agent_prompt == "Handle triggered automation job.");
    REQUIRE(second->execution.job.agent_prompt == "Handle triggered automation job.");
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

TEST_CASE("WebhookProducer normalizes webhook triggers and preserves payload",
          "[unit][automation][queue][triggered][webhook]") {
  TempDb db{"oran-automation-webhook-producer"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto runtime = co_await automation::AutomationRuntime::open(
        io.get_executor(),
        automation::AutomationRuntimeOptions{.database_path = db.string()});
    REQUIRE(runtime.has_value());
    REQUIRE((co_await runtime->repository().upsert_triggered_job(automation::UpsertTriggeredJobRequest{
                 .job_key = "triggered:webhook-ci",
                 .trigger_key = "webhook:ci",
                 .agent_key = "researcher",
                 .agent_prompt = "Handle triggered automation job.",
             }))
                .has_value());

    auto service = runtime->automation_service();
    automation::WebhookProducer producer{service};
    auto produced = co_await producer.trigger(automation::WebhookTriggerRequest{
        .webhook_key = "ci",
        .payload = std::string{R"({"workflow":"build","status":"failed"})"},
        .received_at = at(180s),
        .job_limit = 10,
    });

    REQUIRE(produced.has_value());
    REQUIRE(produced->trigger_key == "webhook:ci");
    REQUIRE(produced->enqueue.intake.trigger_key == "webhook:ci");
    REQUIRE(produced->enqueue.intake.trigger_payload == R"({"workflow":"build","status":"failed"})");
    REQUIRE(produced->enqueue.enqueued_count == 1);
    REQUIRE(service.triggered_queue_size() == 1);

    auto run = co_await service.run_cycle(automation::AutomationServiceCycleRequest{
        .now = at(240s),
        .max_iterations = 1,
        .cron_handler = [](automation::CronDueJob) -> async::Awaitable<core::Result<void>> {
          co_return core::Result<void>{};
        },
        .triggered_handler = [](automation::TriggeredExecutionJob execution) -> async::Awaitable<core::Result<void>> {
          REQUIRE(execution.trigger_key == "webhook:ci");
          REQUIRE(execution.trigger_payload == R"({"workflow":"build","status":"failed"})");
          co_return core::Result<void>{};
        },
        .triggered_max_jobs = 10,
    });

    REQUIRE(run.has_value());
    REQUIRE(run->triggered.completed_count == 1);

    auto prefixed = automation::webhook_trigger_key("webhook:ci");
    REQUIRE_FALSE(prefixed.has_value());
    REQUIRE(prefixed.error().kind() == core::ErrorKind::invalid_argument);
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
                 .agent_prompt = "Handle triggered automation job.",
             }))
                .has_value());
    REQUIRE((co_await repo.upsert_triggered_job(automation::UpsertTriggeredJobRequest{
                 .job_key = "triggered:webhook-ci-b",
                 .trigger_key = "webhook:ci",
                 .agent_key = "coder",
                 .agent_prompt = "Handle triggered automation job.",
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
                 .agent_prompt = "Handle triggered automation job.",
             }))
                .has_value());
    REQUIRE((co_await repo.upsert_triggered_job(automation::UpsertTriggeredJobRequest{
                 .job_key = "triggered:webhook-ci-b",
                 .trigger_key = "webhook:ci",
                 .agent_key = "coder",
                 .agent_prompt = "Handle triggered automation job.",
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

TEST_CASE("TriggeredQueue drops drained descriptors on active triggered agent lease conflicts",
          "[unit][automation][queue][triggered][lease][hook]") {
  TempDb db{"oran-automation-queue-triggered-drain-lease-conflict"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());
    REQUIRE((co_await repo.upsert_triggered_job(automation::UpsertTriggeredJobRequest{
                 .job_key = "triggered:webhook-ci-a",
                 .trigger_key = "webhook:ci",
                 .agent_key = "researcher",
                 .agent_prompt = "Handle triggered automation job.",
             }))
                .has_value());
    auto held = co_await repo.acquire_triggered_agent_lease(automation::AcquireTriggeredAgentLeaseRequest{
        .agent_key = "researcher",
        .owner_key = "owner-b",
        .acquired_at = at(100s),
        .expires_at = at(200s),
    });
    REQUIRE(held.has_value());
    REQUIRE(held->has_value());

    std::vector<hook::JobDroppedPayload> dropped_payloads;
    hook::Bus bus;
    hook::InProcessSink sink{
        "triggered-queue-lease-drop-recorder",
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
                                         .capacity = 4,
                                         .hooks =
                                             automation::TriggeredHookOptions{
                                                 .bus = &bus,
                                                 .source = "triggered-queue",
                                                 .identity = "trigger-loop",
                                             },
                                     }};
    auto enqueued = co_await queue.enqueue(automation::TriggeredQueueEnqueueRequest{
        .trigger_key = "webhook:ci",
        .received_at = at(120s),
        .job_limit = 10,
    });
    REQUIRE(enqueued.has_value());
    REQUIRE(enqueued->enqueued_count == 1);
    REQUIRE(queue.size() == 1);

    int handler_calls{};
    auto drained = co_await queue.drain_once(automation::TriggeredQueueDrainOnceRequest{
        .handler = [&handler_calls](automation::TriggeredExecutionJob) -> async::Awaitable<core::Result<void>> {
          ++handler_calls;
          co_return core::Result<void>{};
        },
        .lease_owner_key = "owner-a",
        .lease_ttl = 60s,
        .blocked_agent_policy = automation::TriggeredQueueBlockedAgentPolicy::drop_on_conflict,
    });

    REQUIRE(drained.has_value());
    REQUIRE_FALSE(drained->execution.completed);
    REQUIRE_FALSE(drained->execution.attempt.completed);
    REQUIRE(drained->dropped.has_value());
    REQUIRE(drained->dropped->reason == automation::TriggeredQueueDropReason::agent_lease_conflict);
    REQUIRE(drained->dropped->execution.job.job_key == "triggered:webhook-ci-a");
    REQUIRE(drained->dropped->queue_capacity == 4);
    REQUIRE(drained->dropped->queue_size == 0);
    REQUIRE(drained->dropped->dropped_at == at(120s));
    REQUIRE(handler_calls == 0);
    REQUIRE(queue.size() == 0);
    REQUIRE(dropped_payloads.size() == 1);

    const auto& payload = dropped_payloads.front();
    REQUIRE(payload.source == "triggered-queue");
    REQUIRE(payload.who.identity == "trigger-loop");
    REQUIRE(payload.job_key == "triggered:webhook-ci-a");
    REQUIRE(payload.who.agent_key == "researcher");
    REQUIRE(payload.trigger_key == "webhook:ci");
    REQUIRE(payload.reason == "agent_lease_conflict");
    REQUIRE(payload.scheduled_at == at(120s));
    REQUIRE(payload.dropped_at == at(120s));
    REQUIRE(payload.queue_capacity == 4);
    REQUIRE(payload.queue_size == 0);

    auto runs = co_await repo.list_triggered_runs(automation::ListTriggeredRunsOptions{
        .job_key = "triggered:webhook-ci-a",
        .limit = 10,
    });
    REQUIRE(runs.has_value());
    REQUIRE(runs->empty());

    auto still_held = co_await repo.acquire_triggered_agent_lease(automation::AcquireTriggeredAgentLeaseRequest{
        .agent_key = "researcher",
        .owner_key = "owner-c",
        .acquired_at = at(121s),
        .expires_at = at(240s),
    });
    REQUIRE(still_held.has_value());
    REQUIRE_FALSE(still_held->has_value());
  });
}

TEST_CASE("TriggeredQueue drains available queued descriptors without waiting",
          "[unit][automation][queue][triggered]") {
  TempDb db{"oran-automation-queue-triggered-drain-available"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());
    REQUIRE((co_await repo.upsert_triggered_job(automation::UpsertTriggeredJobRequest{
                 .job_key = "triggered:webhook-ci-a",
                 .trigger_key = "webhook:ci",
                 .agent_key = "researcher",
                 .agent_prompt = "Handle triggered automation job.",
             }))
                .has_value());
    REQUIRE((co_await repo.upsert_triggered_job(automation::UpsertTriggeredJobRequest{
                 .job_key = "triggered:webhook-ci-b",
                 .trigger_key = "webhook:ci",
                 .agent_key = "coder",
                 .agent_prompt = "Handle triggered automation job.",
             }))
                .has_value());

    automation::TriggeredQueue queue{io.get_executor(),
                                     automation::TriggeredService{repo},
                                     automation::TriggeredQueueOptions{.capacity = 4}};

    auto empty = co_await queue.drain_available(automation::TriggeredQueueDrainAvailableRequest{
        .handler = [](automation::TriggeredExecutionJob) -> async::Awaitable<core::Result<void>> {
          co_return core::Result<void>{};
        },
        .max_jobs = 10,
    });
    REQUIRE(empty.has_value());
    REQUIRE(empty->stop_reason == automation::TriggeredQueueDrainAvailableStopReason::queue_empty);
    REQUIRE(empty->drained_count == 0);
    REQUIRE(empty->drains.empty());

    auto enqueued = co_await queue.enqueue(automation::TriggeredQueueEnqueueRequest{
        .trigger_key = "webhook:ci",
        .received_at = at(120s),
        .job_limit = 10,
    });
    REQUIRE(enqueued.has_value());
    REQUIRE(enqueued->enqueued_count == 2);

    std::vector<std::string> handled;
    auto first_batch = co_await queue.drain_available(automation::TriggeredQueueDrainAvailableRequest{
        .handler = [&handled](automation::TriggeredExecutionJob execution) -> async::Awaitable<core::Result<void>> {
          handled.push_back(execution.job.job_key);
          co_return core::Result<void>{};
        },
        .max_jobs = 1,
    });
    REQUIRE(first_batch.has_value());
    REQUIRE(first_batch->stop_reason == automation::TriggeredQueueDrainAvailableStopReason::max_jobs);
    REQUIRE(first_batch->drained_count == 1);
    REQUIRE(first_batch->completed_count == 1);
    REQUIRE(first_batch->failed_count == 0);
    REQUIRE(first_batch->dropped_count == 0);
    REQUIRE(first_batch->drains.size() == 1);
    REQUIRE(first_batch->drains.front().execution.completed);
    REQUIRE(first_batch->drains.front().queued.execution.job.job_key == handled.front());
    REQUIRE(queue.size() == 1);

    auto second_batch = co_await queue.drain_available(automation::TriggeredQueueDrainAvailableRequest{
        .handler = [&handled](automation::TriggeredExecutionJob execution) -> async::Awaitable<core::Result<void>> {
          handled.push_back(execution.job.job_key);
          co_return core::Result<void>{};
        },
        .max_jobs = 10,
    });
    REQUIRE(second_batch.has_value());
    REQUIRE(second_batch->stop_reason == automation::TriggeredQueueDrainAvailableStopReason::queue_empty);
    REQUIRE(second_batch->drained_count == 1);
    REQUIRE(second_batch->completed_count == 1);
    REQUIRE(second_batch->failed_count == 0);
    REQUIRE(second_batch->dropped_count == 0);
    REQUIRE(queue.size() == 0);
    REQUIRE(handled.size() == 2);

    auto runs_a = co_await repo.list_triggered_runs(automation::ListTriggeredRunsOptions{
        .job_key = "triggered:webhook-ci-a",
        .limit = 10,
    });
    auto runs_b = co_await repo.list_triggered_runs(automation::ListTriggeredRunsOptions{
        .job_key = "triggered:webhook-ci-b",
        .limit = 10,
    });
    REQUIRE(runs_a.has_value());
    REQUIRE(runs_b.has_value());
    REQUIRE(runs_a->size() == 1);
    REQUIRE(runs_b->size() == 1);
  });
}

TEST_CASE("TriggeredQueue drain_available counts handler failures and lease-conflict drops",
          "[unit][automation][queue][triggered][lease]") {
  TempDb db{"oran-automation-queue-triggered-drain-available-outcomes"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());
    REQUIRE((co_await repo.upsert_triggered_job(automation::UpsertTriggeredJobRequest{
                 .job_key = "triggered:webhook-ci-a",
                 .trigger_key = "webhook:ci",
                 .agent_key = "researcher",
                 .agent_prompt = "Handle triggered automation job.",
             }))
                .has_value());
    REQUIRE((co_await repo.upsert_triggered_job(automation::UpsertTriggeredJobRequest{
                 .job_key = "triggered:webhook-ci-b",
                 .trigger_key = "webhook:ci",
                 .agent_key = "coder",
                 .agent_prompt = "Handle triggered automation job.",
             }))
                .has_value());
    auto held = co_await repo.acquire_triggered_agent_lease(automation::AcquireTriggeredAgentLeaseRequest{
        .agent_key = "researcher",
        .owner_key = "owner-b",
        .acquired_at = at(100s),
        .expires_at = at(200s),
    });
    REQUIRE(held.has_value());
    REQUIRE(held->has_value());

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

    std::vector<std::string> handler_calls;
    auto drained = co_await queue.drain_available(automation::TriggeredQueueDrainAvailableRequest{
        .handler =
            [&handler_calls](automation::TriggeredExecutionJob execution) -> async::Awaitable<core::Result<void>> {
          handler_calls.push_back(execution.job.job_key);
          co_return std::unexpected(core::Error::upstream("triggered handler failed"));
        },
        .max_jobs = 10,
        .lease_owner_key = "owner-a",
        .lease_ttl = 60s,
    });

    REQUIRE(drained.has_value());
    REQUIRE(drained->stop_reason == automation::TriggeredQueueDrainAvailableStopReason::queue_empty);
    REQUIRE(drained->drained_count == 2);
    REQUIRE(drained->completed_count == 0);
    REQUIRE(drained->failed_count == 1);
    REQUIRE(drained->dropped_count == 1);
    REQUIRE(drained->drains.size() == 2);
    REQUIRE(handler_calls.size() == 1);
    REQUIRE(queue.size() == 0);

    auto dropped = std::ranges::find_if(drained->drains, [](const auto& item) { return item.dropped.has_value(); });
    REQUIRE(dropped != drained->drains.end());
    REQUIRE(dropped->dropped->reason == automation::TriggeredQueueDropReason::agent_lease_conflict);
    REQUIRE(dropped->queued.execution.job.agent_key == "researcher");

    auto failed = std::ranges::find_if(drained->drains, [](const auto& item) {
      return !item.execution.completed && !item.dropped.has_value();
    });
    REQUIRE(failed != drained->drains.end());
    REQUIRE(failed->queued.execution.job.agent_key == "coder");
    REQUIRE(failed->execution.attempt.error.has_value());

    auto researcher_runs = co_await repo.list_triggered_runs(automation::ListTriggeredRunsOptions{
        .job_key = "triggered:webhook-ci-a",
        .limit = 10,
    });
    auto coder_runs = co_await repo.list_triggered_runs(automation::ListTriggeredRunsOptions{
        .job_key = "triggered:webhook-ci-b",
        .limit = 10,
    });
    REQUIRE(researcher_runs.has_value());
    REQUIRE(coder_runs.has_value());
    REQUIRE(researcher_runs->empty());
    REQUIRE(coder_runs->size() == 1);
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

    auto bad_drain_ttl = co_await queue.drain_once(automation::TriggeredQueueDrainOnceRequest{
        .handler = [](automation::TriggeredExecutionJob) -> async::Awaitable<core::Result<void>> {
          co_return core::Result<void>{};
        },
        .lease_owner_key = "owner-a",
        .lease_ttl = 0s,
    });
    REQUIRE_FALSE(bad_drain_ttl.has_value());
    REQUIRE(bad_drain_ttl.error().kind() == core::ErrorKind::invalid_argument);

    auto bad_blocked_policy = co_await queue.drain_once(automation::TriggeredQueueDrainOnceRequest{
        .handler = [](automation::TriggeredExecutionJob) -> async::Awaitable<core::Result<void>> {
          co_return core::Result<void>{};
        },
        .blocked_agent_policy = static_cast<automation::TriggeredQueueBlockedAgentPolicy>(255),
    });
    REQUIRE_FALSE(bad_blocked_policy.has_value());
    REQUIRE(bad_blocked_policy.error().kind() == core::ErrorKind::invalid_argument);

    auto unsupported_requeue = co_await queue.drain_once(automation::TriggeredQueueDrainOnceRequest{
        .handler = [](automation::TriggeredExecutionJob) -> async::Awaitable<core::Result<void>> {
          co_return core::Result<void>{};
        },
        .blocked_agent_policy = automation::TriggeredQueueBlockedAgentPolicy::requeue_on_conflict,
    });
    REQUIRE_FALSE(unsupported_requeue.has_value());
    REQUIRE(unsupported_requeue.error().kind() == core::ErrorKind::invalid_argument);

    auto empty_max_jobs = co_await queue.drain_available(automation::TriggeredQueueDrainAvailableRequest{
        .handler = [](automation::TriggeredExecutionJob) -> async::Awaitable<core::Result<void>> {
          co_return core::Result<void>{};
        },
        .max_jobs = 0,
    });
    REQUIRE_FALSE(empty_max_jobs.has_value());
    REQUIRE(empty_max_jobs.error().kind() == core::ErrorKind::invalid_argument);

    auto unsupported_requeue_batch = co_await queue.drain_available(automation::TriggeredQueueDrainAvailableRequest{
        .handler = [](automation::TriggeredExecutionJob) -> async::Awaitable<core::Result<void>> {
          co_return core::Result<void>{};
        },
        .max_jobs = 1,
        .blocked_agent_policy = automation::TriggeredQueueBlockedAgentPolicy::requeue_on_conflict,
    });
    REQUIRE_FALSE(unsupported_requeue_batch.has_value());
    REQUIRE(unsupported_requeue_batch.error().kind() == core::ErrorKind::invalid_argument);
  });
}
