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
    REQUIRE(opened->migration_report().current_version == 2);
    REQUIRE(opened->migration_report().applied_versions == std::vector<std::int64_t>{1, 2});

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
    REQUIRE(first->migration_report().current_version == 2);

    auto second =
        co_await automation::AutomationRuntime::open(io.get_executor(),
                                                     automation::AutomationRuntimeOptions{.database_path = db_path});
    REQUIRE(second.has_value());
    REQUIRE(second->migration_report().previous_version == 2);
    REQUIRE(second->migration_report().current_version == 2);
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
