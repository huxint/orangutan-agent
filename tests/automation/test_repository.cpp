// tests/automation/test_repository.cpp - automation repository coverage.

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/automation.hpp>
#include <oran/core/error.hpp>
#include <oran/storage.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace automation = orangutan::automation;
namespace core = orangutan::core;
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

[[nodiscard]] bool has_field(const core::Error& error, std::string_view field) {
  return std::ranges::any_of(error.context(),
                             [field](const auto& entry) { return entry.first == "field" && entry.second == field; });
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

}  // namespace

TEST_CASE("AutomationRepository::migrate applies the automation schema once", "[unit][automation][repository]") {
  TempDb db{"oran-automation-repo-migrate"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};

    auto first = co_await repo.migrate();
    REQUIRE(first.has_value());
    REQUIRE(first->previous_version == 0);
    REQUIRE(first->current_version == 2);
    REQUIRE(first->applied_versions == std::vector<std::int64_t>{1, 2});

    auto second = co_await repo.migrate();
    REQUIRE(second.has_value());
    REQUIRE(second->previous_version == 2);
    REQUIRE(second->current_version == 2);
    REQUIRE(second->applied_versions.empty());
  });
}

TEST_CASE("AutomationRepository round-trips memory retention jobs", "[unit][automation][repository]") {
  TempDb db{"oran-automation-repo-job"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());

    auto upserted = co_await repo.upsert_memory_retention_job(automation::UpsertMemoryRetentionJobRequest{
        .job_key = "memory-retention:cli",
        .job = make_job(),
        .state = automation::PeriodicJobState{.last_fired_at = at(30s)},
    });
    REQUIRE(upserted.has_value());
    REQUIRE(upserted->job_key == "memory-retention:cli");
    REQUIRE(upserted->job == make_job());
    REQUIRE(upserted->state.last_fired_at == at(30s));
    REQUIRE_FALSE(upserted->created_at.empty());
    REQUIRE_FALSE(upserted->updated_at.empty());

    auto loaded = co_await repo.get_memory_retention_job("memory-retention:cli");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    REQUIRE((*loaded)->job_key == upserted->job_key);
    REQUIRE((*loaded)->job == upserted->job);
    REQUIRE((*loaded)->state == upserted->state);

    auto missing = co_await repo.get_memory_retention_job("missing");
    REQUIRE(missing.has_value());
    REQUIRE_FALSE(missing->has_value());
  });
}

TEST_CASE("AutomationRepository updates stored retention policy and last-fired state",
          "[unit][automation][repository]") {
  TempDb db{"oran-automation-repo-update"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());
    REQUIRE((co_await repo.upsert_memory_retention_job(automation::UpsertMemoryRetentionJobRequest{
                 .job_key = "memory-retention:cli",
                 .job = make_job(),
             }))
                .has_value());

    auto changed = make_job("cli-alt");
    changed.policy.max_records_per_scope = 42;
    changed.policy.decay_check_interval = 12h;
    auto upserted = co_await repo.upsert_memory_retention_job(automation::UpsertMemoryRetentionJobRequest{
        .job_key = "memory-retention:cli",
        .job = changed,
        .state = automation::PeriodicJobState{.last_fired_at = at(120s)},
    });
    REQUIRE(upserted.has_value());
    REQUIRE(upserted->job == changed);
    REQUIRE(upserted->state.last_fired_at == at(120s));

    auto marked = co_await repo.mark_memory_retention_fired("memory-retention:cli", at(180s));
    REQUIRE(marked.has_value());
    REQUIRE(marked->state.last_fired_at == at(180s));

    auto missing = co_await repo.mark_memory_retention_fired("missing", at(180s));
    REQUIRE_FALSE(missing.has_value());
    REQUIRE(missing.error().kind() == core::ErrorKind::not_found);
  });
}

TEST_CASE("AutomationRepository records and lists memory retention runs", "[unit][automation][repository]") {
  TempDb db{"oran-automation-repo-runs"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());
    REQUIRE((co_await repo.upsert_memory_retention_job(automation::UpsertMemoryRetentionJobRequest{
                 .job_key = "memory-retention:cli",
                 .job = make_job(),
             }))
                .has_value());

    auto first = co_await repo.record_memory_retention_run(automation::RecordMemoryRetentionRunRequest{
        .job_key = "memory-retention:cli",
        .fired_at = at(120s),
        .finished_at = at(121s),
        .success = true,
        .shadowed_count = 2,
    });
    REQUIRE(first.has_value());
    REQUIRE(first->job_key == "memory-retention:cli");
    REQUIRE(first->success);
    REQUIRE(first->shadowed_count == 2);
    REQUIRE_FALSE(first->error_message.has_value());

    auto second = co_await repo.record_memory_retention_run(automation::RecordMemoryRetentionRunRequest{
        .job_key = "memory-retention:cli",
        .fired_at = at(180s),
        .finished_at = at(181s),
        .success = false,
        .error_message = "backend unavailable",
    });
    REQUIRE(second.has_value());
    REQUIRE_FALSE(second->success);
    REQUIRE(second->error_message == "backend unavailable");

    auto listed = co_await repo.list_memory_retention_runs(automation::ListMemoryRetentionRunsOptions{
        .job_key = "memory-retention:cli",
        .limit = 10,
    });
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 2);
    REQUIRE((*listed)[0].id == second->id);
    REQUIRE((*listed)[1].id == first->id);

    auto capped = co_await repo.list_memory_retention_runs(automation::ListMemoryRetentionRunsOptions{
        .job_key = "memory-retention:cli",
        .limit = 1,
    });
    REQUIRE(capped.has_value());
    REQUIRE(capped->size() == 1);
    REQUIRE((*capped)[0].id == second->id);
  });
}

TEST_CASE("AutomationRepository acquires, expires, and releases memory retention leases",
          "[unit][automation][repository][lease]") {
  TempDb db{"oran-automation-repo-lease"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());
    REQUIRE((co_await repo.upsert_memory_retention_job(automation::UpsertMemoryRetentionJobRequest{
                 .job_key = "memory-retention:cli",
                 .job = make_job(),
             }))
                .has_value());

    auto first = co_await repo.acquire_memory_retention_lease(automation::AcquireMemoryRetentionLeaseRequest{
        .job_key = "memory-retention:cli",
        .owner_key = "owner-a",
        .acquired_at = at(100s),
        .expires_at = at(160s),
    });
    REQUIRE(first.has_value());
    REQUIRE(first->has_value());
    REQUIRE((*first)->job_key == "memory-retention:cli");
    REQUIRE((*first)->owner_key == "owner-a");
    REQUIRE((*first)->acquired_at == at(100s));
    REQUIRE((*first)->expires_at == at(160s));
    REQUIRE_FALSE((*first)->updated_at.empty());

    auto active_competitor =
        co_await repo.acquire_memory_retention_lease(automation::AcquireMemoryRetentionLeaseRequest{
            .job_key = "memory-retention:cli",
            .owner_key = "owner-b",
            .acquired_at = at(120s),
            .expires_at = at(180s),
        });
    REQUIRE(active_competitor.has_value());
    REQUIRE_FALSE(active_competitor->has_value());

    auto expired_takeover = co_await repo.acquire_memory_retention_lease(automation::AcquireMemoryRetentionLeaseRequest{
        .job_key = "memory-retention:cli",
        .owner_key = "owner-b",
        .acquired_at = at(161s),
        .expires_at = at(220s),
    });
    REQUIRE(expired_takeover.has_value());
    REQUIRE(expired_takeover->has_value());
    REQUIRE((*expired_takeover)->owner_key == "owner-b");
    REQUIRE((*expired_takeover)->acquired_at == at(161s));
    REQUIRE((*expired_takeover)->expires_at == at(220s));

    auto wrong_owner = co_await repo.release_memory_retention_lease(
        automation::ReleaseMemoryRetentionLeaseRequest{.job_key = "memory-retention:cli", .owner_key = "owner-a"});
    REQUIRE(wrong_owner.has_value());
    REQUIRE_FALSE(*wrong_owner);

    auto released = co_await repo.release_memory_retention_lease(
        automation::ReleaseMemoryRetentionLeaseRequest{.job_key = "memory-retention:cli", .owner_key = "owner-b"});
    REQUIRE(released.has_value());
    REQUIRE(*released);

    auto reacquired = co_await repo.acquire_memory_retention_lease(automation::AcquireMemoryRetentionLeaseRequest{
        .job_key = "memory-retention:cli",
        .owner_key = "owner-c",
        .acquired_at = at(180s),
        .expires_at = at(240s),
    });
    REQUIRE(reacquired.has_value());
    REQUIRE(reacquired->has_value());
    REQUIRE((*reacquired)->owner_key == "owner-c");
  });
}

TEST_CASE("AutomationRepository validates memory retention persistence inputs", "[unit][automation][repository]") {
  TempDb db{"oran-automation-repo-validation"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    automation::AutomationRepository repo{pool};
    REQUIRE((co_await repo.migrate()).has_value());

    auto invalid_job_key = co_await repo.upsert_memory_retention_job(automation::UpsertMemoryRetentionJobRequest{
        .job_key = "",
        .job = make_job(),
    });
    REQUIRE_FALSE(invalid_job_key.has_value());
    REQUIRE(invalid_job_key.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(has_field(invalid_job_key.error(), "job_key"));

    auto invalid_policy = co_await repo.upsert_memory_retention_job(automation::UpsertMemoryRetentionJobRequest{
        .job_key = "memory-retention:cli",
        .job = make_job(""),
    });
    REQUIRE_FALSE(invalid_policy.has_value());
    REQUIRE(has_field(invalid_policy.error(), "scope_key"));

    auto missing_error = co_await repo.record_memory_retention_run(automation::RecordMemoryRetentionRunRequest{
        .job_key = "memory-retention:cli",
        .fired_at = at(10s),
        .finished_at = at(11s),
        .success = false,
    });
    REQUIRE_FALSE(missing_error.has_value());
    REQUIRE(has_field(missing_error.error(), "error_message"));

    auto bad_order = co_await repo.record_memory_retention_run(automation::RecordMemoryRetentionRunRequest{
        .job_key = "memory-retention:cli",
        .fired_at = at(12s),
        .finished_at = at(11s),
    });
    REQUIRE_FALSE(bad_order.has_value());
    REQUIRE(has_field(bad_order.error(), "finished_at"));

    auto bad_limit = co_await repo.list_memory_retention_runs(automation::ListMemoryRetentionRunsOptions{
        .job_key = "memory-retention:cli",
        .limit = 0,
    });
    REQUIRE_FALSE(bad_limit.has_value());
    REQUIRE(has_field(bad_limit.error(), "limit"));

    auto invalid_lease_owner =
        co_await repo.acquire_memory_retention_lease(automation::AcquireMemoryRetentionLeaseRequest{
            .job_key = "memory-retention:cli",
            .owner_key = "",
            .acquired_at = at(20s),
            .expires_at = at(21s),
        });
    REQUIRE_FALSE(invalid_lease_owner.has_value());
    REQUIRE(has_field(invalid_lease_owner.error(), "owner_key"));

    auto invalid_lease_expiry =
        co_await repo.acquire_memory_retention_lease(automation::AcquireMemoryRetentionLeaseRequest{
            .job_key = "memory-retention:cli",
            .owner_key = "owner-a",
            .acquired_at = at(20s),
            .expires_at = at(20s),
        });
    REQUIRE_FALSE(invalid_lease_expiry.has_value());
    REQUIRE(has_field(invalid_lease_expiry.error(), "expires_at"));

    auto missing_lease_job =
        co_await repo.acquire_memory_retention_lease(automation::AcquireMemoryRetentionLeaseRequest{
            .job_key = "missing",
            .owner_key = "owner-a",
            .acquired_at = at(20s),
            .expires_at = at(21s),
        });
    REQUIRE_FALSE(missing_lease_job.has_value());
    REQUIRE(missing_lease_job.error().kind() == core::ErrorKind::not_found);
  });
}
