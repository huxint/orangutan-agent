#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <asio/any_io_executor.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>
#include <oran/tool/output.hpp>

namespace orangutan::tool {
class Registry;
struct DispatchContext;
}  // namespace orangutan::tool

namespace orangutan::agent {

/// Runtime knobs documented in `docs/design-docs/tool-runtime.md`.
/// Defaults match the spec: 4 concurrent tools, 60 s per-call timeout, 5 min
/// idle lock TTL. `idle_lock_ttl` is parsed and stored now so slice 117 can
/// consume the same value without a config rev.
struct ToolSchedulerOptions {
  std::size_t max_parallel_tools{4};
  std::chrono::milliseconds per_call_timeout{60'000};
  std::chrono::milliseconds idle_lock_ttl{300'000};

  friend bool operator==(const ToolSchedulerOptions&, const ToolSchedulerOptions&) = default;
};

/// One tool call in a provider-emitted batch. `tool_use_id` is the assistant
/// `ToolUseContent::id` the agent loop must round-trip back into a
/// `ToolResultContent` block; the scheduler carries it through unchanged so
/// ordered results re-attach to the right provider id.
struct ToolBatchCall {
  std::string tool_use_id;
  std::string name;
  std::string input_json;
};

/// One result row in original batch order. `output` is whatever the registry
/// would have returned — including model-repairable errors, infrastructure
/// errors, and (slice 116) per-call timeout / parent-cancellation cancellations.
struct ToolBatchResult {
  std::string tool_use_id;
  std::string name;
  core::Result<tool::Output> output;
};

/// Snapshot of the per-canonical-path read/write lock table.
/// `--explain-rules`-style debug surfaces consume this through
/// `ToolScheduler::lock_stats()` before `oran-log` exists. All counters are
/// monotonic over the scheduler's lifetime; `current_entries` reflects the
/// post-reap snapshot.
struct ToolSchedulerLockStats {
  std::uint64_t shared_acquires{0};
  std::uint64_t exclusive_acquires{0};
  std::uint64_t contended_acquires{0};
  std::uint64_t cancelled_acquires{0};
  std::uint64_t reaped_entries{0};
  std::size_t current_entries{0};
  std::size_t peak_entries{0};

  friend bool operator==(const ToolSchedulerLockStats&, const ToolSchedulerLockStats&) = default;
};

/// Bounded-parallel `tool::Registry::dispatch` driver. The scheduler is owned
/// by the `agent::Loop` once slice 120 wires it through; slice 116 ships the
/// type so direct tests can pin AC1, AC2, AC6, and the partial AC5 contracts
/// before the loop changes shape.
class ToolScheduler {
public:
  /// Borrow the registry for the scheduler's lifetime. The registry stays
  /// single-threaded by design (see `docs/design-docs/tool-runtime.md`
  /// "Scheduler Boundary"); the scheduler dispatches concurrently into it
  /// because `Registry::dispatch` is `const` and the per-call
  /// `DispatchContext` is brace-initialised fresh per call. Long-lived
  /// services hung off the prototype context (audit, hook bus, broker)
  /// remain responsible for their own concurrency story; `StorageAuditSink`
  /// and the SQLite `Pool` already serialise writes internally.
  ToolScheduler(asio::any_io_executor executor, tool::Registry& registry, ToolSchedulerOptions options = {});
  ~ToolScheduler();

  ToolScheduler(const ToolScheduler&) = delete;
  ToolScheduler& operator=(const ToolScheduler&) = delete;
  ToolScheduler(ToolScheduler&&) noexcept;
  ToolScheduler& operator=(ToolScheduler&&) noexcept;

  /// Run a batch of tool calls under bounded parallelism. `prototype` holds
  /// the references/pointers to the long-lived dispatch services
  /// (`permission::RuleSet`, `permission::AuditSink`, optional broker / hook
  /// bus / workspace, scope/agent/identity strings, output caps, and the
  /// optional `parent_turn_id`). The scheduler creates a fresh per-call
  /// context with `tool::DispatchContext::for_now(prototype, ...)`, rebinding
  /// those references and refreshing `now` so broker TTL and approval TTL
  /// checks see the real per-call clock. Returns the ordered result vector
  /// even when execution finishes out of order; an empty batch
  /// returns an empty vector without touching the registry.
  ///
  /// Parent cancellation: the caller may bind a cancellation slot to this
  /// coroutine. When it fires, the scheduler emits on a per-batch
  /// cancellation signal that propagates to every in-flight dispatch, drains
  /// remaining completions with the parent slot temporarily filtered to
  /// `disable_cancellation()`, and returns `Error::cancelled` with
  /// `reason=parent_cancelled`. Children that already filled a result keep
  /// it; children that were cancelled mid-flight contribute their cancelled
  /// result rows but the batch-level error is what the caller sees. The
  /// full 100 ms guarantee (AC5) and the `cancellation_lag` audit kind land
  /// in slice 119.
  [[nodiscard]] async::Awaitable<core::Result<std::vector<ToolBatchResult>>>
  run_batch(std::vector<ToolBatchCall> batch, tool::DispatchContext& prototype);

  /// Join dispatches that outlived a cancelled batch before releasing their
  /// borrowed contexts and services. A context selects only its own calls;
  /// null joins every call. Ignores cancellation while draining.
  [[nodiscard]] async::Awaitable<core::Result<void>> wait_idle(const tool::DispatchContext* context = nullptr);

  [[nodiscard]] const ToolSchedulerOptions& options() const noexcept;

  /// Snapshot of the per-canonical-path lock table. Cheap to call; copies a
  /// small POD aggregate.
  [[nodiscard]] ToolSchedulerLockStats lock_stats() const noexcept;

  /// Drop idle lock-table entries whose idle age exceeds
  /// `ToolSchedulerOptions::idle_lock_ttl`. Returns the number of entries
  /// removed. Slice 117 ships this as an explicit caller-driven primitive; a
  /// future slice will own a periodic background tick that calls it
  /// (`docs/design-docs/tool-runtime.md` AC10).
  std::size_t reap_idle_locks(core::Time now);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace orangutan::agent
