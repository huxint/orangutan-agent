// tests/agent/test_scheduler.cpp — ToolScheduler coverage (slices 116-117).
//
// Slice 116 ships bounded parallelism (AC1), ordered results (AC2), per-call
// timeout (AC6), and the first parent-cancellation surface (partial AC5).
// Slice 117 adds the per-canonical-path read/write lock table behind
// `agent::ToolScheduler` (AC3, AC4, AC10).
// Approval gating, audit fan-out, the full 100 ms cancel guarantee, and
// `cancellation_lag` audit naming move in later slices and get their own
// tests there.

#include <oran/agent.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/cancellation_state.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/this_coro.hpp>
#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/core/error.hpp>
#include <oran/core/time.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/hook.hpp>
#include <oran/permission.hpp>
#include <oran/tool.hpp>

#include "../../src/oran-agent/_impl/path_lock_table.hpp"
#include "../test-helpers/run_async.hpp"

namespace agent = orangutan::agent;
namespace async = orangutan::async;
namespace core = orangutan::core;
namespace hook = orangutan::hook;
namespace permission = orangutan::permission;
namespace test = orangutan::tests;
namespace tool = orangutan::tool;

namespace {

using namespace std::chrono_literals;

[[nodiscard]] bool error_has_context(const core::Error& error, std::string_view key, std::string_view value) {
  for (const auto& [k, v] : error.context()) {
    if (k == key && v == value) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] core::ToolDef tool_def(std::string name) {
  return core::ToolDef{
      .name = std::move(name),
      .description = "Latency-controllable test tool",
      .input_schema_json = R"({"type":"object","properties":{},"additionalProperties":true})",
      .required_capabilities = {},
      .deferred = false,
      .category = "test",
  };
}

[[nodiscard]] permission::RuleSet allow_all_rules() {
  permission::RuleSet rules;
  rules.add(permission::Rule{
      .verdict = permission::Verdict::allow,
      .tool_pattern = "*",
      .capability = std::nullopt,
  });
  return rules;
}

struct ConcurrencyTracker {
  std::atomic<int> in_flight{0};
  std::atomic<int> peak{0};

  void enter() noexcept {
    const auto current = in_flight.fetch_add(1, std::memory_order_relaxed) + 1;
    auto observed = peak.load(std::memory_order_relaxed);
    while (current > observed && !peak.compare_exchange_weak(observed, current, std::memory_order_relaxed)) {
      // peak races between observation and update; retry until either we
      // raise the bar or someone else already raised it past `current`.
    }
  }
  void leave() noexcept {
    in_flight.fetch_sub(1, std::memory_order_relaxed);
  }
};

/// Register a handler that sleeps for `latency` and returns `done:<input>`.
void add_latency_tool(tool::Registry& registry,
                      std::string name,
                      std::chrono::milliseconds latency,
                      std::shared_ptr<ConcurrencyTracker> tracker = {}) {
  auto handler = [latency, tracker = std::move(tracker)](
                     std::string_view input,
                     tool::DispatchContext& ctx) -> async::Awaitable<core::Result<tool::Output>> {
    if (tracker) {
      tracker->enter();
    }
    auto sleep_result = co_await async::sleep_for(ctx.executor, latency);
    if (tracker) {
      tracker->leave();
    }
    if (!sleep_result) {
      co_return std::unexpected(sleep_result.error());
    }
    co_return tool::Output::text_only(std::string{"done:"} + std::string{input});
  };
  REQUIRE(registry.add(tool_def(std::move(name)), std::move(handler)).has_value());
}

/// Register a handler that sleeps for `latency` and then returns an
/// infrastructure error. Used to prove a failing call still records its audit
/// decision row and still emits `tool_after` (AC7's failure-path clause).
void add_failing_tool(tool::Registry& registry, std::string name, std::chrono::milliseconds latency) {
  auto handler = [latency](std::string_view,
                           tool::DispatchContext& ctx) -> async::Awaitable<core::Result<tool::Output>> {
    auto sleep_result = co_await async::sleep_for(ctx.executor, latency);
    if (!sleep_result) {
      co_return std::unexpected(sleep_result.error());
    }
    co_return std::unexpected(core::Error::internal("fake tool: deliberate handler failure"));
  };
  REQUIRE(registry.add(tool_def(std::move(name)), std::move(handler)).has_value());
}

/// Register a handler that sleeps for `latency` and returns text plus a
/// non-empty `Output::usage`. A successful capped result with measured usage is
/// what triggers slice-67 same-row audit metadata enrichment.
void add_usage_tool(tool::Registry& registry,
                    std::string name,
                    std::chrono::milliseconds latency,
                    tool::ToolUsage usage) {
  auto handler = [latency, usage](std::string_view,
                                  tool::DispatchContext& ctx) -> async::Awaitable<core::Result<tool::Output>> {
    auto sleep_result = co_await async::sleep_for(ctx.executor, latency);
    if (!sleep_result) {
      co_return std::unexpected(sleep_result.error());
    }
    auto output = tool::Output::text_only("done");
    output.usage = usage;
    co_return output;
  };
  REQUIRE(registry.add(tool_def(std::move(name)), std::move(handler)).has_value());
}

/// Register a handler that DELIBERATELY ignores its cancellation slot: it
/// disables cancellation for its own coroutine, then sleeps. Models the "tool
/// bug" spec 0012 AC5 carves out — a handler that never polls its cancellation
/// state. Under parent cancellation the scheduler's 100 ms guarantee must still
/// hold (`run_batch` returns promptly without waiting for this handler) and the
/// offending tool must be named in a `cancellation_lag` audit row.
void add_uncancellable_tool(tool::Registry& registry, std::string name, std::chrono::milliseconds latency) {
  auto handler = [latency](std::string_view,
                           tool::DispatchContext& ctx) -> async::Awaitable<core::Result<tool::Output>> {
    co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
    [[maybe_unused]] auto slept = co_await async::sleep_for(ctx.executor, latency);
    co_return tool::Output::text_only("done-uncancellable");
  };
  REQUIRE(registry.add(tool_def(std::move(name)), std::move(handler)).has_value());
}

[[nodiscard]] permission::ApprovalBroker make_broker() {
  auto broker = permission::ApprovalBroker::with_random_secret();
  REQUIRE(broker.has_value());
  return std::move(*broker);
}

[[nodiscard]] permission::ApprovalToken
grant(permission::ApprovalBroker& broker, std::string_view tool_name, std::string_view input, core::Time now) {
  return broker.approve(
      permission::ApprovalGrant{
          .tool_name = tool_name,
          .input = input,
          .identity = "operator-1",
          .ttl = std::chrono::seconds{60},
          .replay_max = 4,
      },
      now);
}

[[nodiscard]] tool::DispatchContext
make_prototype(asio::io_context& io, permission::RuleSet& rules, permission::AuditSink& audit) {
  return tool::DispatchContext{
      .executor = io.get_executor(),
      .mode = permission::Mode::default_,
      .rules = rules,
      .audit = audit,
      .scope_key = "scope-A",
      .agent_key = "coder",
      .identity = "operator-1",
  };
}

[[nodiscard]] agent::ToolBatchCall call(std::size_t index, std::string name, std::string input_json = "{}") {
  return agent::ToolBatchCall{
      .tool_use_id = std::string{"call-"} + std::to_string(index),
      .name = std::move(name),
      .input_json = std::move(input_json),
  };
}

class TempDir {
public:
  explicit TempDir(std::string name)
      : path_{std::filesystem::temp_directory_path() /
              (std::move(name) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))} {
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

void touch_file(const std::filesystem::path& path) {
  std::ofstream out{path};
  REQUIRE(out.good());
}

struct Interval {
  std::chrono::steady_clock::time_point start{};
  std::chrono::steady_clock::time_point end{};
};

[[nodiscard]] bool overlaps(const Interval& a, const Interval& b) {
  return a.start < b.end && b.start < a.end;
}

struct LockTracker {
  std::mutex mu;
  std::vector<std::pair<std::string, Interval>> intervals;
  std::atomic<int> in_flight{0};
  std::atomic<int> peak{0};

  void enter() noexcept {
    const auto current = in_flight.fetch_add(1, std::memory_order_relaxed) + 1;
    auto observed = peak.load(std::memory_order_relaxed);
    while (current > observed && !peak.compare_exchange_weak(observed, current, std::memory_order_relaxed)) {}
  }
  void leave() noexcept {
    in_flight.fetch_sub(1, std::memory_order_relaxed);
  }
  void record(std::string key, Interval iv) {
    std::scoped_lock l{mu};
    intervals.emplace_back(std::move(key), iv);
  }
  [[nodiscard]] std::vector<Interval> for_key(std::string_view key) {
    std::scoped_lock l{mu};
    std::vector<Interval> out;
    for (const auto& [k, iv] : intervals) {
      if (k == key) {
        out.push_back(iv);
      }
    }
    return out;
  }
};

[[nodiscard]] core::ToolDef tracked_tool_def(std::string name, std::vector<core::Capability> caps) {
  return core::ToolDef{
      .name = std::move(name),
      .description = "Lock-tracking fixture tool",
      .input_schema_json =
          R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"],"additionalProperties":true})",
      .required_capabilities = std::move(caps),
      .deferred = false,
      .category = "test",
  };
}

void add_tracked_tool(tool::Registry& registry,
                      std::string name,
                      std::vector<core::Capability> caps,
                      std::chrono::milliseconds latency,
                      std::shared_ptr<LockTracker> tracker) {
  auto handler = [latency, tracker = std::move(tracker)](
                     std::string_view input,
                     tool::DispatchContext& ctx) -> async::Awaitable<core::Result<tool::Output>> {
    tracker->enter();
    const auto start = std::chrono::steady_clock::now();
    auto sleep_result = co_await async::sleep_for(ctx.executor, latency);
    const auto end = std::chrono::steady_clock::now();
    tracker->leave();
    // Fake tools live outside the registry's hard-coded built-in name set,
    // so `ctx.resolved_path` is never populated for them. Track by the raw
    // input bytes — two calls targeting the same path share an input string,
    // which is what the tests assert on.
    tracker->record(std::string{input}, Interval{.start = start, .end = end});
    if (!sleep_result) {
      co_return std::unexpected(sleep_result.error());
    }
    co_return tool::Output::text_only(std::string{"done"});
  };
  REQUIRE(registry.add(tracked_tool_def(std::move(name), std::move(caps)), std::move(handler)).has_value());
}

[[nodiscard]] tool::Workspace make_workspace(const std::filesystem::path& root) {
  auto workspace = tool::Workspace::create(root.string());
  REQUIRE(workspace.has_value());
  return std::move(*workspace);
}

[[nodiscard]] tool::DispatchContext make_prototype_with_workspace(asio::io_context& io,
                                                                  permission::RuleSet& rules,
                                                                  permission::AuditSink& audit,
                                                                  tool::Workspace& workspace) {
  return tool::DispatchContext{
      .executor = io.get_executor(),
      .mode = permission::Mode::default_,
      .rules = rules,
      .audit = audit,
      .workspace = &workspace,
      .scope_key = "scope-A",
      .agent_key = "coder",
      .identity = "operator-1",
  };
}

[[nodiscard]] std::string write_call_input(std::string_view rel_path) {
  std::string out;
  out.reserve(rel_path.size() + 20);
  out += R"({"path":")";
  out.append(rel_path);
  out += R"("})";
  return out;
}

}  // namespace

TEST_CASE("ToolScheduler: empty batch returns an empty result vector", "[unit][agent][scheduler]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    auto rules = allow_all_rules();
    permission::NullAuditSink audit;
    auto prototype = make_prototype(io, rules, audit);

    agent::ToolScheduler scheduler{io.get_executor(), registry};
    auto result = co_await scheduler.run_batch({}, prototype);

    REQUIRE(result.has_value());
    REQUIRE(result->empty());
  });
}

TEST_CASE("ToolScheduler: single call returns one ordered result with the registry output",
          "[unit][agent][scheduler]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    add_latency_tool(registry, "fake.noop", 0ms);
    auto rules = allow_all_rules();
    permission::NullAuditSink audit;
    auto prototype = make_prototype(io, rules, audit);

    agent::ToolScheduler scheduler{io.get_executor(), registry};

    std::vector<agent::ToolBatchCall> batch;
    batch.push_back(call(0, "fake.noop", R"({"k":"v"})"));

    auto result = co_await scheduler.run_batch(std::move(batch), prototype);
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 1);
    REQUIRE((*result)[0].tool_use_id == "call-0");
    REQUIRE((*result)[0].name == "fake.noop");
    REQUIRE((*result)[0].output.has_value());
    REQUIRE((*result)[0].output->text == R"(done:{"k":"v"})");
  });
}

TEST_CASE("ToolScheduler: bounded parallelism caps in-flight calls", "[unit][agent][scheduler][parallelism]") {
  test::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        constexpr std::size_t kBatch = 10;
        constexpr std::size_t kMaxParallel = 4;
        constexpr auto kLatency = 50ms;

        auto tracker = std::make_shared<ConcurrencyTracker>();
        tool::Registry registry;
        add_latency_tool(registry, "fake.latency", kLatency, tracker);
        auto rules = allow_all_rules();
        permission::NullAuditSink audit;
        auto prototype = make_prototype(io, rules, audit);

        agent::ToolScheduler scheduler{io.get_executor(),
                                       registry,
                                       agent::ToolSchedulerOptions{
                                           .max_parallel_tools = kMaxParallel,
                                           .per_call_timeout = 30s,
                                       }};

        std::vector<agent::ToolBatchCall> batch;
        batch.reserve(kBatch);
        for (std::size_t i = 0; i < kBatch; ++i) {
          batch.push_back(call(i, "fake.latency"));
        }

        const auto started = std::chrono::steady_clock::now();
        auto result = co_await scheduler.run_batch(std::move(batch), prototype);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);

        REQUIRE(result.has_value());
        REQUIRE(result->size() == kBatch);
        REQUIRE(tracker->peak.load() <= static_cast<int>(kMaxParallel));
        REQUIRE(tracker->peak.load() > 0);
        REQUIRE(elapsed >= 3 * kLatency / 2);  // AC1 lower bound: ceil(10/4) * 50ms = 150 ms.
        // Upper bound is lenient to absorb CI scheduler noise on WSL2.
        REQUIRE(elapsed <= 10 * kLatency);
      },
      2s);
}

TEST_CASE("ToolScheduler: results are returned in original tool_use order", "[unit][agent][scheduler][ordering]") {
  test::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        tool::Registry registry;
        add_latency_tool(registry, "fake.slow", 60ms);
        add_latency_tool(registry, "fake.fast", 5ms);
        auto rules = allow_all_rules();
        permission::NullAuditSink audit;
        auto prototype = make_prototype(io, rules, audit);

        agent::ToolScheduler scheduler{io.get_executor(),
                                       registry,
                                       agent::ToolSchedulerOptions{.max_parallel_tools = 4, .per_call_timeout = 5s}};

        std::vector<agent::ToolBatchCall> batch;
        batch.push_back(call(0, "fake.slow"));
        batch.push_back(call(1, "fake.fast"));
        batch.push_back(call(2, "fake.slow"));
        batch.push_back(call(3, "fake.fast"));

        auto result = co_await scheduler.run_batch(std::move(batch), prototype);
        REQUIRE(result.has_value());
        REQUIRE(result->size() == 4);
        REQUIRE((*result)[0].tool_use_id == "call-0");
        REQUIRE((*result)[0].name == "fake.slow");
        REQUIRE((*result)[1].tool_use_id == "call-1");
        REQUIRE((*result)[1].name == "fake.fast");
        REQUIRE((*result)[2].tool_use_id == "call-2");
        REQUIRE((*result)[2].name == "fake.slow");
        REQUIRE((*result)[3].tool_use_id == "call-3");
        REQUIRE((*result)[3].name == "fake.fast");
        // All four returned the canned `done:<input>` payload regardless of
        // the order in which they actually finished.
        for (const auto& row : *result) {
          REQUIRE(row.output.has_value());
          REQUIRE(row.output->text == "done:{}");
        }
      },
      2s);
}

TEST_CASE("ToolScheduler: per-call timeout surfaces Error::cancelled with reason=timeout",
          "[unit][agent][scheduler][timeout]") {
  test::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        tool::Registry registry;
        add_latency_tool(registry, "fake.slow", 500ms);
        auto rules = allow_all_rules();
        permission::NullAuditSink audit;
        auto prototype = make_prototype(io, rules, audit);

        agent::ToolScheduler scheduler{io.get_executor(),
                                       registry,
                                       agent::ToolSchedulerOptions{
                                           .max_parallel_tools = 4,
                                           .per_call_timeout = 50ms,
                                       }};

        std::vector<agent::ToolBatchCall> batch;
        batch.push_back(call(0, "fake.slow"));

        auto result = co_await scheduler.run_batch(std::move(batch), prototype);
        REQUIRE(result.has_value());
        REQUIRE(result->size() == 1);
        const auto& row = (*result)[0];
        REQUIRE(row.tool_use_id == "call-0");
        REQUIRE_FALSE(row.output.has_value());
        REQUIRE(row.output.error().kind() == core::ErrorKind::cancelled);
        REQUIRE(error_has_context(row.output.error(), "reason", "timeout"));
        REQUIRE(error_has_context(row.output.error(), "tool", "fake.slow"));
        REQUIRE(error_has_context(row.output.error(), "per_call_timeout_ms", "50"));
      },
      2s);
}

TEST_CASE("ToolScheduler: parent cancellation propagates to in-flight calls",
          "[unit][agent][scheduler][cancellation]") {
  asio::io_context io;
  asio::cancellation_signal signal;

  tool::Registry registry;
  add_latency_tool(registry, "fake.slow", 1s);
  auto rules = allow_all_rules();
  permission::NullAuditSink audit;
  auto prototype = make_prototype(io, rules, audit);

  agent::ToolScheduler scheduler{io.get_executor(),
                                 registry,
                                 agent::ToolSchedulerOptions{
                                     .max_parallel_tools = 4,
                                     .per_call_timeout = 30s,
                                 }};

  std::vector<agent::ToolBatchCall> batch;
  batch.push_back(call(0, "fake.slow"));
  batch.push_back(call(1, "fake.slow"));

  std::optional<core::Result<std::vector<agent::ToolBatchResult>>> result;
  std::exception_ptr failure;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<std::vector<agent::ToolBatchResult>>> {
        co_return co_await scheduler.run_batch(std::move(batch), prototype);
      },
      asio::bind_cancellation_slot(signal.slot(),
                                   [&](std::exception_ptr ep, core::Result<std::vector<agent::ToolBatchResult>> r) {
                                     failure = ep;
                                     result = std::move(r);
                                   }));

  // Let the scheduler spawn its children before we cancel the parent.
  asio::post(io, [&] { asio::post(io, [&] { signal.emit(asio::cancellation_type::terminal); }); });

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!result.has_value() && !failure && std::chrono::steady_clock::now() < deadline) {
    io.run_one();
  }
  io.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
  REQUIRE(error_has_context(result->error(), "reason", "parent_cancelled"));
}

TEST_CASE("ToolScheduler: two writes to the same canonical path serialize (AC3)", "[unit][agent][scheduler][lock]") {
  test::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        TempDir root{"oran-sched-ac3"};
        const auto rel = std::string{"target.txt"};
        touch_file(root.path() / rel);

        auto tracker = std::make_shared<LockTracker>();
        tool::Registry registry;
        add_tracked_tool(registry, "fake.lock.write", {core::Capability::write_file}, 80ms, tracker);

        auto workspace = make_workspace(root.path());
        auto rules = allow_all_rules();
        permission::NullAuditSink audit;
        auto prototype = make_prototype_with_workspace(io, rules, audit, workspace);

        agent::ToolScheduler scheduler{io.get_executor(),
                                       registry,
                                       agent::ToolSchedulerOptions{.max_parallel_tools = 4, .per_call_timeout = 10s}};

        std::vector<agent::ToolBatchCall> batch;
        batch.push_back(call(0, "fake.lock.write", write_call_input(rel)));
        batch.push_back(call(1, "fake.lock.write", write_call_input(rel)));

        auto result = co_await scheduler.run_batch(std::move(batch), prototype);
        REQUIRE(result.has_value());
        REQUIRE(result->size() == 2);
        REQUIRE((*result)[0].output.has_value());
        REQUIRE((*result)[1].output.has_value());

        const auto intervals = tracker->for_key(write_call_input(rel));
        REQUIRE(intervals.size() == 2);
        REQUIRE_FALSE(overlaps(intervals[0], intervals[1]));
        REQUIRE(tracker->peak.load() == 1);

        // Both calls were exclusive acquires; the second one waited.
        const auto stats = scheduler.lock_stats();
        REQUIRE(stats.exclusive_acquires == 2);
        REQUIRE(stats.shared_acquires == 0);
        REQUIRE(stats.contended_acquires >= 1);
        REQUIRE(stats.peak_entries >= 1);
      },
      5s);
}

TEST_CASE("ToolScheduler: concurrent read and write on the same path obey the read/write lock (AC4)",
          "[unit][agent][scheduler][lock]") {
  test::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        TempDir root{"oran-sched-ac4"};
        const auto rel = std::string{"shared.txt"};
        touch_file(root.path() / rel);

        auto tracker = std::make_shared<LockTracker>();
        tool::Registry registry;
        add_tracked_tool(registry, "fake.lock.read", {core::Capability::read_file}, 80ms, tracker);
        add_tracked_tool(registry, "fake.lock.write", {core::Capability::write_file}, 80ms, tracker);

        auto workspace = make_workspace(root.path());
        auto rules = allow_all_rules();
        permission::NullAuditSink audit;
        auto prototype = make_prototype_with_workspace(io, rules, audit, workspace);

        agent::ToolScheduler scheduler{io.get_executor(),
                                       registry,
                                       agent::ToolSchedulerOptions{.max_parallel_tools = 4, .per_call_timeout = 10s}};

        std::vector<agent::ToolBatchCall> batch;
        batch.push_back(call(0, "fake.lock.read", write_call_input(rel)));
        batch.push_back(call(1, "fake.lock.write", write_call_input(rel)));

        auto result = co_await scheduler.run_batch(std::move(batch), prototype);
        REQUIRE(result.has_value());
        REQUIRE(result->size() == 2);
        REQUIRE((*result)[0].output.has_value());
        REQUIRE((*result)[1].output.has_value());

        const auto intervals = tracker->for_key(write_call_input(rel));
        REQUIRE(intervals.size() == 2);
        REQUIRE_FALSE(overlaps(intervals[0], intervals[1]));
        REQUIRE(tracker->peak.load() == 1);

        const auto stats = scheduler.lock_stats();
        REQUIRE(stats.shared_acquires == 1);
        REQUIRE(stats.exclusive_acquires == 1);
      },
      5s);
}

TEST_CASE("ToolScheduler: two reads to the same path run concurrently under the shared lock",
          "[unit][agent][scheduler][lock]") {
  test::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        TempDir root{"oran-sched-shared"};
        const auto rel = std::string{"shared-read.txt"};
        touch_file(root.path() / rel);

        auto tracker = std::make_shared<LockTracker>();
        tool::Registry registry;
        add_tracked_tool(registry, "fake.lock.read", {core::Capability::read_file}, 80ms, tracker);

        auto workspace = make_workspace(root.path());
        auto rules = allow_all_rules();
        permission::NullAuditSink audit;
        auto prototype = make_prototype_with_workspace(io, rules, audit, workspace);

        agent::ToolScheduler scheduler{io.get_executor(),
                                       registry,
                                       agent::ToolSchedulerOptions{.max_parallel_tools = 4, .per_call_timeout = 10s}};

        std::vector<agent::ToolBatchCall> batch;
        batch.push_back(call(0, "fake.lock.read", write_call_input(rel)));
        batch.push_back(call(1, "fake.lock.read", write_call_input(rel)));

        auto result = co_await scheduler.run_batch(std::move(batch), prototype);
        REQUIRE(result.has_value());
        REQUIRE(result->size() == 2);
        REQUIRE(tracker->peak.load() == 2);

        const auto stats = scheduler.lock_stats();
        REQUIRE(stats.shared_acquires == 2);
        REQUIRE(stats.exclusive_acquires == 0);
        REQUIRE(stats.contended_acquires == 0);
      },
      5s);
}

TEST_CASE("ToolScheduler: writes to different paths run concurrently", "[unit][agent][scheduler][lock]") {
  test::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        TempDir root{"oran-sched-distinct"};
        touch_file(root.path() / "a.txt");
        touch_file(root.path() / "b.txt");

        auto tracker = std::make_shared<LockTracker>();
        tool::Registry registry;
        add_tracked_tool(registry, "fake.lock.write", {core::Capability::write_file}, 80ms, tracker);

        auto workspace = make_workspace(root.path());
        auto rules = allow_all_rules();
        permission::NullAuditSink audit;
        auto prototype = make_prototype_with_workspace(io, rules, audit, workspace);

        agent::ToolScheduler scheduler{io.get_executor(),
                                       registry,
                                       agent::ToolSchedulerOptions{.max_parallel_tools = 4, .per_call_timeout = 10s}};

        std::vector<agent::ToolBatchCall> batch;
        batch.push_back(call(0, "fake.lock.write", write_call_input("a.txt")));
        batch.push_back(call(1, "fake.lock.write", write_call_input("b.txt")));

        auto result = co_await scheduler.run_batch(std::move(batch), prototype);
        REQUIRE(result.has_value());
        REQUIRE(result->size() == 2);
        REQUIRE(tracker->peak.load() == 2);

        const auto stats = scheduler.lock_stats();
        REQUIRE(stats.exclusive_acquires == 2);
        REQUIRE(stats.contended_acquires == 0);
        REQUIRE(stats.current_entries == 2);
      },
      5s);
}

TEST_CASE("ToolScheduler: capability-free tools skip the path lock", "[unit][agent][scheduler][lock]") {
  test::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        TempDir root{"oran-sched-nocap"};
        touch_file(root.path() / "c.txt");

        auto tracker = std::make_shared<LockTracker>();
        tool::Registry registry;
        // No capability => no lock class, no path resolution required.
        add_tracked_tool(registry, "fake.lock.noop", {}, 30ms, tracker);

        auto workspace = make_workspace(root.path());
        auto rules = allow_all_rules();
        permission::NullAuditSink audit;
        auto prototype = make_prototype_with_workspace(io, rules, audit, workspace);

        agent::ToolScheduler scheduler{io.get_executor(),
                                       registry,
                                       agent::ToolSchedulerOptions{.max_parallel_tools = 4, .per_call_timeout = 5s}};

        std::vector<agent::ToolBatchCall> batch;
        batch.push_back(call(0, "fake.lock.noop", write_call_input("c.txt")));
        batch.push_back(call(1, "fake.lock.noop", write_call_input("c.txt")));

        auto result = co_await scheduler.run_batch(std::move(batch), prototype);
        REQUIRE(result.has_value());
        REQUIRE(tracker->peak.load() == 2);

        const auto stats = scheduler.lock_stats();
        REQUIRE(stats.shared_acquires == 0);
        REQUIRE(stats.exclusive_acquires == 0);
        REQUIRE(stats.current_entries == 0);
      },
      5s);
}

TEST_CASE("ToolScheduler: reap_idle_locks drops idle entries past the TTL", "[unit][agent][scheduler][lock][reap]") {
  test::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        TempDir root{"oran-sched-reap"};
        touch_file(root.path() / "d.txt");

        auto tracker = std::make_shared<LockTracker>();
        tool::Registry registry;
        add_tracked_tool(registry, "fake.lock.write", {core::Capability::write_file}, 5ms, tracker);

        auto workspace = make_workspace(root.path());
        auto rules = allow_all_rules();
        permission::NullAuditSink audit;
        auto prototype = make_prototype_with_workspace(io, rules, audit, workspace);

        agent::ToolScheduler scheduler{
            io.get_executor(),
            registry,
            agent::ToolSchedulerOptions{.max_parallel_tools = 4, .per_call_timeout = 5s, .idle_lock_ttl = 100ms}};

        std::vector<agent::ToolBatchCall> batch;
        batch.push_back(call(0, "fake.lock.write", write_call_input("d.txt")));

        auto result = co_await scheduler.run_batch(std::move(batch), prototype);
        REQUIRE(result.has_value());

        const auto pre_reap = scheduler.lock_stats();
        REQUIRE(pre_reap.current_entries == 1);

        // Reap with a now far in the past relative to the entry's idle stamp:
        // nothing should be evicted.
        REQUIRE(scheduler.reap_idle_locks(core::Time::epoch()) == 0);
        REQUIRE(scheduler.lock_stats().current_entries == 1);

        // Reap with a future-enough time: the lone entry is evicted.
        const auto future = core::Time{std::chrono::system_clock::now() + std::chrono::seconds{10}};
        const auto evicted = scheduler.reap_idle_locks(future);
        REQUIRE(evicted == 1);
        const auto post_reap = scheduler.lock_stats();
        REQUIRE(post_reap.current_entries == 0);
        REQUIRE(post_reap.reaped_entries == 1);
      },
      5s);
}

TEST_CASE("PathLockTable: 10 000 distinct paths reap to empty after the idle TTL (AC10)",
          "[unit][agent][scheduler][lock][reap]") {
  test::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        constexpr std::size_t kPaths = 10'000;
        const auto idle_ttl = std::chrono::milliseconds{200};
        agent::detail::PathLockTable table{agent::detail::PathLockTableOptions{.idle_ttl = idle_ttl}};

        // The lock guard's destructor reads `core::time::now_utc()` to stamp
        // `idle_since`, so use wall-clock time on both sides. The reap
        // deadline below is far enough in the future that every entry — even
        // ones released later in the loop — falls past the TTL.
        for (std::size_t i = 0; i < kPaths; ++i) {
          auto guard = co_await table.acquire(io.get_executor(),
                                              "/synthetic/lock/" + std::to_string(i),
                                              agent::detail::PathLockMode::exclusive,
                                              core::time::now_utc());
          REQUIRE(guard.has_value());
          // Guard drops at scope exit → release runs synchronously and stamps
          // `idle_since` with the real wall clock.
        }

        REQUIRE(table.stats().current_entries == kPaths);

        const auto reap_at = core::Time{std::chrono::system_clock::now() + idle_ttl + std::chrono::seconds{1}};
        const auto evicted = table.reap(reap_at);
        REQUIRE(evicted == kPaths);
        REQUIRE(table.stats().current_entries == 0);
        REQUIRE(table.stats().reaped_entries == kPaths);
      },
      30s);
}

TEST_CASE("PathLockTable: cancellation during a contended wait does not orphan the lock",
          "[unit][agent][scheduler][lock][cancellation]") {
  asio::io_context io;
  agent::detail::PathLockTable table{agent::detail::PathLockTableOptions{.idle_ttl = 1s}};
  const auto now = core::Time{std::chrono::system_clock::now()};

  std::optional<agent::detail::PathLockGuard> holder;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        auto result =
            co_await table.acquire(io.get_executor(), "/contended/path", agent::detail::PathLockMode::exclusive, now);
        REQUIRE(result.has_value());
        holder = std::move(*result);
      },
      asio::detached);
  io.run();
  REQUIRE(holder.has_value());

  // Spawn a waiter that contends on the held lock, then cancel it.
  asio::cancellation_signal waiter_signal;
  io.restart();
  std::optional<core::Result<agent::detail::PathLockGuard>> waiter_result;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<agent::detail::PathLockGuard>> {
        co_return co_await table.acquire(io.get_executor(),
                                         "/contended/path",
                                         agent::detail::PathLockMode::exclusive,
                                         now);
      },
      asio::bind_cancellation_slot(
          waiter_signal.slot(),
          [&](std::exception_ptr, core::Result<agent::detail::PathLockGuard> r) { waiter_result = std::move(r); }));

  // Pump the io_context once so the waiter actually enters its wait state.
  asio::post(io, [&] { waiter_signal.emit(asio::cancellation_type::terminal); });
  io.run();
  REQUIRE(waiter_result.has_value());
  REQUIRE_FALSE(waiter_result->has_value());
  REQUIRE(waiter_result->error().kind() == core::ErrorKind::cancelled);

  // Release the original holder: a fresh acquire on the same key must succeed
  // because the cancelled waiter did not leave an orphaned permit.
  holder.reset();
  io.restart();
  std::optional<core::Result<agent::detail::PathLockGuard>> recovered;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<agent::detail::PathLockGuard>> {
        co_return co_await table.acquire(io.get_executor(),
                                         "/contended/path",
                                         agent::detail::PathLockMode::exclusive,
                                         now);
      },
      [&](std::exception_ptr, core::Result<agent::detail::PathLockGuard> r) { recovered = std::move(r); });
  io.run();
  REQUIRE(recovered.has_value());
  REQUIRE(recovered->has_value());

  const auto stats = table.stats();
  REQUIRE(stats.cancelled_acquires >= 1);
}

// ---------------------------------------------------------------------------
// Slice 118 — approval gating + audit/hook fan-out correctness under
// parallelism (most of AC7). The scheduler must preserve every per-call
// invariant `tool::Registry::dispatch` guarantees for a single call when it
// fans a batch out concurrently: exactly N audit rows, exactly N `tool_after`
// publishes (failures included), per-call approval resolution, and slice-67
// same-row usage enrichment with no cross-talk between identical calls.
// ---------------------------------------------------------------------------

TEST_CASE("ToolScheduler: a batch records exactly N audit rows and N tool_after publishes (AC7)",
          "[unit][agent][scheduler][audit]") {
  test::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        tool::Registry registry;
        add_latency_tool(registry, "fake.ok", 40ms);
        add_failing_tool(registry, "fake.fail", 10ms);
        auto rules = allow_all_rules();
        permission::RecordingAuditSink audit;

        // Count `tool_after` publishes through an advisory in-process sink.
        auto after_publishes = std::make_shared<std::atomic<int>>(0);
        hook::InProcessSink after_sink{
            "after-counter",
            [after_publishes](hook::Event, hook::Payload) -> async::Awaitable<core::Result<void>> {
              after_publishes->fetch_add(1, std::memory_order_relaxed);
              co_return core::Result<void>{};
            }};
        hook::Bus bus;
        bus.bind(after_sink, {hook::Event::tool_after});

        auto prototype = make_prototype(io, rules, audit);
        prototype.bus = &bus;

        agent::ToolScheduler scheduler{io.get_executor(),
                                       registry,
                                       agent::ToolSchedulerOptions{.max_parallel_tools = 4, .per_call_timeout = 10s}};

        // Mixed success/failure with varied latency so completions interleave
        // out of original order — AC7 must hold "regardless of execution order".
        std::vector<agent::ToolBatchCall> batch;
        batch.push_back(call(0, "fake.ok"));
        batch.push_back(call(1, "fake.fail"));
        batch.push_back(call(2, "fake.ok"));
        batch.push_back(call(3, "fake.fail"));

        auto result = co_await scheduler.run_batch(std::move(batch), prototype);
        REQUIRE(result.has_value());
        REQUIRE(result->size() == 4);

        // Exactly N permission-decision rows. The bus carries no blocking
        // `tool_before` sink, so the blocking publish consults nobody and no
        // `hook_publish` row is appended.
        REQUIRE(audit.events().size() == 4);
        for (const auto& event : audit.events()) {
          REQUIRE(event.event_kind == "permission_decision");
        }

        // Exactly N `tool_after` publishes, including the two failing calls.
        REQUIRE(after_publishes->load() == 4);

        // Ordered results preserve the per-call success/failure pattern.
        REQUIRE((*result)[0].output.has_value());
        REQUIRE_FALSE((*result)[1].output.has_value());
        REQUIRE((*result)[2].output.has_value());
        REQUIRE_FALSE((*result)[3].output.has_value());
      },
      5s);
}

TEST_CASE("ToolScheduler: identical concurrent calls each enrich their own audit row (slice 67 under parallelism)",
          "[unit][agent][scheduler][audit]") {
  test::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        tool::Registry registry;
        // Non-zero usage so a successful result drives slice-67 enrichment to
        // write a non-"{}" metadata blob the assertions below can detect.
        add_usage_tool(registry, "fake.usage", 30ms, tool::ToolUsage{.bytes_read = 7, .files_touched = 1});
        auto rules = allow_all_rules();
        permission::RecordingAuditSink audit;

        auto prototype = make_prototype(io, rules, audit);
        // A shared trace turn id gives both rows the slice-79 correlation key;
        // the same-row matcher must still pair each enrichment to a distinct row.
        core::TurnId turn{};
        turn[0] = std::byte{0x11};
        prototype.parent_turn_id = turn;

        agent::ToolScheduler scheduler{io.get_executor(),
                                       registry,
                                       agent::ToolSchedulerOptions{.max_parallel_tools = 4, .per_call_timeout = 10s}};

        // Two byte-identical calls: same tool + same input => same input_hash,
        // same identity/scope/agent/turn, same initial "{}" metadata. The
        // decision rows are indistinguishable by the slice-67 match key except
        // for the previous_metadata_json hook that lets each enrichment consume
        // exactly one not-yet-enriched row. If that hook fails under
        // parallelism, both updates land on the same row and the other stays
        // "{}" — which the per-row assertion below would catch.
        std::vector<agent::ToolBatchCall> batch;
        batch.push_back(call(0, "fake.usage", R"({"path":"same"})"));
        batch.push_back(call(1, "fake.usage", R"({"path":"same"})"));

        auto result = co_await scheduler.run_batch(std::move(batch), prototype);
        REQUIRE(result.has_value());
        REQUIRE(result->size() == 2);
        REQUIRE((*result)[0].output.has_value());
        REQUIRE((*result)[1].output.has_value());

        // Exactly two rows: enrichment updates in place and never appends a
        // second decision row (AC7 holds even when calls collide).
        REQUIRE(audit.events().size() == 2);
        // Both rows carry the parent turn id and were enriched with usage — no
        // row left stale, no cross-talk.
        for (const auto& event : audit.events()) {
          REQUIRE(event.event_kind == "permission_decision");
          REQUIRE(event.parent_turn_id == turn);
          REQUIRE(event.metadata_json != "{}");
          REQUIRE(event.metadata_json.contains("\"usage\""));
          REQUIRE(event.metadata_json.contains("bytes_read"));
        }
      },
      5s);
}

TEST_CASE("ToolScheduler: ask calls resolve per call and a denied call is not hidden",
          "[unit][agent][scheduler][approval]") {
  test::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        tool::Registry registry;
        add_latency_tool(registry, "fake.ask", 20ms);
        permission::RuleSet rules;
        rules.add(permission::Rule{
            .verdict = permission::Verdict::ask,
            .tool_pattern = "fake.ask",
            .capability = std::nullopt,
        });
        permission::RecordingAuditSink audit;

        // The broker holds a grant for one specific input. The scheduler
        // refreshes `now` to the wall clock per call, so grant at the real
        // clock keeps the 60 s TTL valid through dispatch.
        auto broker = make_broker();
        const auto now = core::time::now_utc();
        constexpr std::string_view approved_input = R"({"k":"approve"})";
        const auto token = grant(broker, "fake.ask", approved_input, now);

        auto prototype = make_prototype(io, rules, audit);
        prototype.approval_broker = &broker;
        prototype.approval_token = &token;

        agent::ToolScheduler scheduler{io.get_executor(),
                                       registry,
                                       agent::ToolSchedulerOptions{.max_parallel_tools = 4, .per_call_timeout = 10s}};

        // call 0 matches the broker grant -> approved -> handler runs.
        // call 1 carries a different input the grant does not cover -> rejected.
        // Both are `ask`; each must resolve through the broker on its own slot.
        std::vector<agent::ToolBatchCall> batch;
        batch.push_back(call(0, "fake.ask", std::string{approved_input}));
        batch.push_back(call(1, "fake.ask", R"({"k":"unauthorized"})"));

        auto result = co_await scheduler.run_batch(std::move(batch), prototype);
        REQUIRE(result.has_value());
        REQUIRE(result->size() == 2);

        // Ordered results: the approved call ran; the unauthorized call surfaces
        // as permission_denied at its own index rather than being hidden behind
        // the successful call.
        REQUIRE((*result)[0].output.has_value());
        REQUIRE((*result)[0].output->text == R"(done:{"k":"approve"})");
        REQUIRE_FALSE((*result)[1].output.has_value());
        REQUIRE((*result)[1].output.error().kind() == core::ErrorKind::permission_denied);
        REQUIRE(error_has_context((*result)[1].output.error(), "tool", "fake.ask"));

        // Each call recorded exactly one decision row carrying its own outcome.
        REQUIRE(audit.events().size() == 2);
        const auto approved = std::ranges::count_if(audit.events(), [](const permission::AuditEvent& e) {
          return e.outcome == permission::AuditOutcome::approved;
        });
        const auto rejected = std::ranges::count_if(audit.events(), [](const permission::AuditEvent& e) {
          return e.outcome == permission::AuditOutcome::rejected;
        });
        REQUIRE(approved == 1);
        REQUIRE(rejected == 1);
      },
      5s);
}

// ---------------------------------------------------------------------------
// Slice 119 — cancellation propagation + `cancellation_lag` audit (AC5). A
// parent cancel must end every *cancel-aware* in-flight call within the 100 ms
// guarantee, and `run_batch` must return promptly even when a tool handler
// ignores its cancellation slot. Such a handler cannot be forced to stop
// (asio cancellation is cooperative; `co_await (dispatch || timeout)` will not
// resolve until the ignoring handler finishes), so the scheduler stops
// *awaiting* it: after the grace window it names the offending tool in a
// `cancellation_lag` audit row and returns, leaving the laggard to wind down
// on its own. A batch of purely cancel-aware tools records no such row.
// ---------------------------------------------------------------------------

TEST_CASE("ToolScheduler: a cancellation-ignoring tool is named as cancellation_lag without stalling the batch (AC5)",
          "[unit][agent][scheduler][cancellation]") {
  asio::io_context io;
  asio::cancellation_signal signal;

  tool::Registry registry;
  // Cancel-aware: its sleep resolves to `cancelled` the moment the scheduler
  // emits, so it winds down well inside the 100 ms grace window.
  add_latency_tool(registry, "fake.cancelaware", 5s);
  // Cancellation-ignoring: runs its full 1 s regardless of the emitted cancel.
  add_uncancellable_tool(registry, "fake.stuck", 1s);
  auto rules = allow_all_rules();
  permission::RecordingAuditSink audit;
  auto prototype = make_prototype(io, rules, audit);

  agent::ToolScheduler scheduler{io.get_executor(),
                                 registry,
                                 agent::ToolSchedulerOptions{.max_parallel_tools = 4, .per_call_timeout = 30s}};

  std::vector<agent::ToolBatchCall> batch;
  batch.push_back(call(0, "fake.cancelaware"));
  batch.push_back(call(1, "fake.stuck"));

  std::optional<core::Result<std::vector<agent::ToolBatchResult>>> result;
  std::exception_ptr failure;
  std::chrono::steady_clock::time_point cancel_at{};
  std::chrono::steady_clock::time_point result_at{};
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<std::vector<agent::ToolBatchResult>>> {
        co_return co_await scheduler.run_batch(std::move(batch), prototype);
      },
      asio::bind_cancellation_slot(signal.slot(),
                                   [&](std::exception_ptr ep, core::Result<std::vector<agent::ToolBatchResult>> r) {
                                     failure = ep;
                                     result = std::move(r);
                                     result_at = std::chrono::steady_clock::now();
                                   }));

  // Let the scheduler spawn its children and enter their handlers before the
  // parent cancellation lands.
  asio::post(io, [&] {
    asio::post(io, [&] {
      cancel_at = std::chrono::steady_clock::now();
      signal.emit(asio::cancellation_type::terminal);
    });
  });

  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!result.has_value() && !failure && std::chrono::steady_clock::now() < deadline) {
    io.run_one();
  }
  // Drain the cancellation-ignoring tool so the stack-scoped registry / audit
  // sink outlive the detached child that is still winding down.
  io.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
  REQUIRE(error_has_context(result->error(), "reason", "parent_cancelled"));

  // 100 ms guarantee: the batch returned without blocking on the 1 s laggard.
  // The 500 ms bound is a 5x margin over the grace window and well under the
  // laggard's own latency, so a regression that re-introduced the unbounded
  // drain would push this past 1 s and fail here.
  REQUIRE(result_at - cancel_at < 500ms);

  // Exactly one `cancellation_lag` row, naming the cancellation-ignoring tool;
  // the cancel-aware tool wound down inside the grace window and is not named.
  const auto lag_rows = std::ranges::count_if(audit.events(), [](const permission::AuditEvent& e) {
    return e.event_kind == "cancellation_lag";
  });
  REQUIRE(lag_rows == 1);
  const auto stuck_named = std::ranges::any_of(audit.events(), [](const permission::AuditEvent& e) {
    return e.event_kind == "cancellation_lag" && e.tool_name == "fake.stuck";
  });
  REQUIRE(stuck_named);
  const auto cancelaware_named = std::ranges::any_of(audit.events(), [](const permission::AuditEvent& e) {
    return e.event_kind == "cancellation_lag" && e.tool_name == "fake.cancelaware";
  });
  REQUIRE_FALSE(cancelaware_named);
  // The row carries the `error_kind=cancellation_lag` marker spec 0012 AC5 names.
  const auto marked = std::ranges::any_of(audit.events(), [](const permission::AuditEvent& e) {
    return e.event_kind == "cancellation_lag" && e.metadata_json.contains("cancellation_lag");
  });
  REQUIRE(marked);
}

TEST_CASE("ToolScheduler: cancel-aware tools record no cancellation_lag rows on parent cancel (AC5)",
          "[unit][agent][scheduler][cancellation]") {
  asio::io_context io;
  asio::cancellation_signal signal;

  tool::Registry registry;
  add_latency_tool(registry, "fake.slow", 1s);
  auto rules = allow_all_rules();
  permission::RecordingAuditSink audit;
  auto prototype = make_prototype(io, rules, audit);

  agent::ToolScheduler scheduler{io.get_executor(),
                                 registry,
                                 agent::ToolSchedulerOptions{.max_parallel_tools = 4, .per_call_timeout = 30s}};

  std::vector<agent::ToolBatchCall> batch;
  batch.push_back(call(0, "fake.slow"));
  batch.push_back(call(1, "fake.slow"));

  std::optional<core::Result<std::vector<agent::ToolBatchResult>>> result;
  std::exception_ptr failure;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<std::vector<agent::ToolBatchResult>>> {
        co_return co_await scheduler.run_batch(std::move(batch), prototype);
      },
      asio::bind_cancellation_slot(signal.slot(),
                                   [&](std::exception_ptr ep, core::Result<std::vector<agent::ToolBatchResult>> r) {
                                     failure = ep;
                                     result = std::move(r);
                                   }));

  asio::post(io, [&] { asio::post(io, [&] { signal.emit(asio::cancellation_type::terminal); }); });

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!result.has_value() && !failure && std::chrono::steady_clock::now() < deadline) {
    io.run_one();
  }
  io.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
  REQUIRE(error_has_context(result->error(), "reason", "parent_cancelled"));

  // Cancel-aware tools all wound down inside the grace window: no tool is a
  // cancellation laggard, so the audit log carries zero `cancellation_lag` rows.
  const auto lag_rows = std::ranges::count_if(audit.events(), [](const permission::AuditEvent& e) {
    return e.event_kind == "cancellation_lag";
  });
  REQUIRE(lag_rows == 0);
}

TEST_CASE("ToolScheduler: a queued call cancelled before it runs is not mis-named as cancellation_lag (AC5)",
          "[unit][agent][scheduler][cancellation]") {
  asio::io_context io;
  asio::cancellation_signal signal;

  tool::Registry registry;
  add_latency_tool(registry, "fake.slow", 1s);  // cancel-aware
  auto rules = allow_all_rules();
  permission::RecordingAuditSink audit;
  auto prototype = make_prototype(io, rules, audit);

  // max_parallel_tools = 2 < batch size, so four of the six calls sit queued on
  // the channel-as-semaphore when the parent cancel lands. A queued call never
  // ran a handler, so it must report its (cancelled) completion rather than be
  // mis-named as a cancellation laggard.
  agent::ToolScheduler scheduler{io.get_executor(),
                                 registry,
                                 agent::ToolSchedulerOptions{.max_parallel_tools = 2, .per_call_timeout = 30s}};

  std::vector<agent::ToolBatchCall> batch;
  for (std::size_t i = 0; i < 6; ++i) {
    batch.push_back(call(i, "fake.slow"));
  }

  std::optional<core::Result<std::vector<agent::ToolBatchResult>>> result;
  std::exception_ptr failure;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<std::vector<agent::ToolBatchResult>>> {
        co_return co_await scheduler.run_batch(std::move(batch), prototype);
      },
      asio::bind_cancellation_slot(signal.slot(),
                                   [&](std::exception_ptr ep, core::Result<std::vector<agent::ToolBatchResult>> r) {
                                     failure = ep;
                                     result = std::move(r);
                                   }));

  asio::post(io, [&] { asio::post(io, [&] { signal.emit(asio::cancellation_type::terminal); }); });

  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!result.has_value() && !failure && std::chrono::steady_clock::now() < deadline) {
    io.run_one();
  }
  io.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
  REQUIRE(error_has_context(result->error(), "reason", "parent_cancelled"));

  // The two running tools are cancel-aware and the four queued ones never
  // started a handler, so none ignored a cancellation slot: zero rows named.
  const auto lag_rows = std::ranges::count_if(audit.events(), [](const permission::AuditEvent& e) {
    return e.event_kind == "cancellation_lag";
  });
  REQUIRE(lag_rows == 0);
}
