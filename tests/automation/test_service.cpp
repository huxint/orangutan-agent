// tests/automation/test_service.cpp - automation service tick coverage.

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
#include <oran/memory/longterm.hpp>
#include <oran/storage.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace automation = orangutan::automation;
namespace core = orangutan::core;
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

}  // namespace

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
    automation::MemoryRetentionService service{repo, backend};

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

    auto runs = co_await repo.list_memory_retention_runs(automation::ListMemoryRetentionRunsOptions{
        .job_key = "memory-retention:cli",
        .limit = 10,
    });
    REQUIRE(runs.has_value());
    REQUIRE(runs->empty());
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
    automation::MemoryRetentionService service{repo, backend};

    auto result = co_await service.tick(automation::MemoryRetentionTickRequest{
        .job_key = "memory-retention:cli",
        .now = at(60s),
    });

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::upstream);
    REQUIRE(backend.decay_calls == 1);

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
