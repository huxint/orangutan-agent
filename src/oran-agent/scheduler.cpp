#include <oran/agent/scheduler.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <asio/any_io_executor.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/cancellation_state.hpp>
#include <asio/cancellation_type.hpp>
#include <asio/co_spawn.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/async/channel.hpp>
#include <oran/async/sleep.hpp>
#include <oran/core/error.hpp>
#include <oran/core/result.hpp>
#include <oran/permission/audit.hpp>
#include <oran/tool/output.hpp>
#include <oran/tool/path-locks.hpp>
#include <oran/tool/registry.hpp>

namespace orangutan::agent {

namespace {

/// Reusable "parent cancelled" error so tests pin a stable contract.
[[nodiscard]] core::Error parent_cancelled_error() {
  return core::Error::cancelled().with("reason", "parent_cancelled");
}

[[nodiscard]] core::Error timeout_error(std::string_view tool_name, std::chrono::milliseconds per_call_timeout) {
  return core::Error::cancelled()
      .with("reason", "timeout")
      .with("tool", std::string{tool_name})
      .with("per_call_timeout_ms", std::to_string(per_call_timeout.count()));
}

// A bounded batch return still requires a context join before services are released.
constexpr std::chrono::milliseconds kCancellationGrace{100};

[[nodiscard]] std::string cancellation_lag_metadata_json(std::chrono::milliseconds per_call_timeout) {
  return std::format(R"({{"error_kind":"cancellation_lag","cancellation_grace_ms":{},"per_call_timeout_ms":{}}})",
                     kCancellationGrace.count(),
                     per_call_timeout.count());
}

struct BatchState {
  asio::any_io_executor executor;
  ToolSchedulerOptions options;
  std::size_t total_calls;
  /// True only for a single-call batch: with no concurrency the prototype's
  /// `approval_token_output` slot is safe to thread through so a blocking
  /// ask-approval can hand the caller its issued token for replay.
  bool thread_approval_token_output;
  std::vector<std::optional<ToolBatchResult>> results;
  async::Channel<std::size_t> completion;
  /// One cancellation signal per spawned child. asio's `cancellation_signal`
  /// owns a single slot at a time, so multiple coroutines binding to one
  /// signal's slot would clobber each other's installed handlers
  /// (the channel's `assign(...)` would overwrite the previous one and the
  /// awaitable cleanup would dereference freed memory). `std::deque` keeps
  /// the signals at stable addresses across `emplace_back`, which the bound
  /// child coroutines and the parent-emitted cancellation both rely on.
  std::deque<asio::cancellation_signal> child_cancels;

  BatchState(asio::any_io_executor exec, std::size_t n, ToolSchedulerOptions opts)
      : executor{std::move(exec)}, options{opts}, total_calls{n}, thread_approval_token_output{n == 1}, results(n),
        completion{executor, n} {
    for (std::size_t i = 0; i < n; ++i) {
      child_cancels.emplace_back();
    }
  }
};

}  // namespace

class ToolScheduler::Impl {
public:
  struct SharedState {
    SharedState(asio::any_io_executor executor, tool::Registry& registry, ToolSchedulerOptions options)
        : executor{std::move(executor)}, registry{&registry}, options{options} {}

    asio::any_io_executor executor;
    tool::Registry* registry;
    ToolSchedulerOptions options;
    tool::PathLocks locks;
    std::size_t active_calls{0};
    std::map<const tool::DispatchContext*, std::size_t> active_contexts;
    std::vector<std::weak_ptr<asio::steady_timer>> idle_waiters;
  };

  Impl(asio::any_io_executor executor, tool::Registry& registry, ToolSchedulerOptions options)
      : state_{std::make_shared<SharedState>(std::move(executor), registry, options)} {}

  [[nodiscard]] async::Awaitable<core::Result<std::vector<ToolBatchResult>>>
  run_batch(std::vector<ToolBatchCall> batch, tool::DispatchContext& prototype) {
    return run_batch_shared(state_, std::move(batch), prototype);
  }

  async::Awaitable<core::Result<void>> wait_idle(const tool::DispatchContext* context) {
    auto state = state_;
    co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
    while (context ? state->active_contexts.contains(context) : state->active_calls != 0) {
      auto waiter = std::make_shared<asio::steady_timer>(state->executor);
      waiter->expires_at(asio::steady_timer::time_point::max());
      state->idle_waiters.push_back(waiter);
      std::error_code error;
      co_await waiter->async_wait(asio::redirect_error(asio::use_awaitable, error));
    }
    co_return core::Result<void>{};
  }

private:
  [[nodiscard]] static async::Awaitable<core::Result<std::vector<ToolBatchResult>>>
  run_batch_shared(std::shared_ptr<SharedState> shared,
                   std::vector<ToolBatchCall> batch,
                   tool::DispatchContext& prototype) {
    if (!shared->executor || shared->options.max_parallel_tools == 0 || shared->options.per_call_timeout.count() <= 0)
      co_return std::unexpected(core::Error::invalid_argument("scheduler requires an executor and positive bounds"));
    if (batch.empty()) {
      co_return std::vector<ToolBatchResult>{};
    }

    if (auto cancel = co_await asio::this_coro::cancellation_state;
        cancel.cancelled() != asio::cancellation_type::none) {
      co_return std::unexpected(parent_cancelled_error());
    }

    auto state = std::make_shared<BatchState>(shared->executor, batch.size(), shared->options);

    std::vector<std::string> names;
    names.reserve(batch.size());
    for (const auto& call : batch)
      names.push_back(call.name);
    std::size_t admitted = 0;
    const auto workers = std::min(batch.size(), shared->options.max_parallel_tools);
    for (; admitted < workers; ++admitted)
      start_call(state, shared, std::move(batch[admitted]), admitted, prototype);

    std::vector<bool> reported(state->total_calls, false);
    std::size_t completed = 0;
    bool parent_cancelled = false;

    // Phase 1 — cancel-sensitive drain. A cancelled `receive()` means the
    // parent token fired; otherwise every call reports its completion index.
    while (completed < state->total_calls) {
      auto next = co_await state->completion.receive();
      if (!next) {
        parent_cancelled = true;
        break;
      }
      reported[*next] = true;
      ++completed;
      if (auto cancellation = co_await asio::this_coro::cancellation_state;
          cancellation.cancelled() != asio::cancellation_type::none) {
        parent_cancelled = true;
        break;
      }
      if (admitted < batch.size()) {
        start_call(state, shared, std::move(batch[admitted]), admitted, prototype);
        ++admitted;
      }
    }

    if (!parent_cancelled) {
      std::vector<ToolBatchResult> ordered;
      ordered.reserve(state->total_calls);
      for (auto& slot : state->results) {
        ordered.emplace_back(std::move(*slot));
      }
      co_return ordered;
    }

    // Phase 2 — parent cancellation. Emit on every child signal so each
    // in-flight dispatch sees its own cancellation contract, then stop
    // honouring the parent token so the bounded drain below can still await.
    // Queued values have no borrowed dispatch frame and cannot lag cleanup.
    for (std::size_t i = admitted; i < state->total_calls; ++i) {
      reported[i] = true;
      ++completed;
    }
    for (std::size_t i = 0; i < admitted; ++i)
      state->child_cancels[i].emit(asio::cancellation_type::all);
    co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());

    // Wait out the cancellation grace window for the remaining calls. A cancel-aware
    // tool resolves almost at once; a tool that ignores its cancellation slot
    // keeps `run_call` suspended in its `dispatch || timeout` race — that race
    // cannot resolve until the handler returns, because asio cancellation is
    // cooperative — so we race the drain against `kCancellationGrace` and stop
    // awaiting at the deadline instead of stalling the whole batch.
    if (completed < state->total_calls) {
      using namespace asio::experimental::awaitable_operators;
      co_await (drain_remaining(state, reported, completed) || async::sleep_for(shared->executor, kCancellationGrace));
    }

    // Name every call that missed the grace window. A laggard keeps running
    // (it holds the shared `BatchState` alive) and will wind down on its own;
    // the audit row records `error_kind=cancellation_lag` against the tool so
    // the offending handler is identifiable.
    for (std::size_t i = 0; i < state->total_calls; ++i) {
      if (!reported[i]) {
        [[maybe_unused]] auto recorded =
            co_await record_cancellation_lag(prototype, names[i], shared->options.per_call_timeout);
      }
    }

    co_return std::unexpected(parent_cancelled_error());
  }

  static void start_call(const std::shared_ptr<BatchState>& state,
                         const std::shared_ptr<SharedState>& shared,
                         ToolBatchCall call,
                         std::size_t index,
                         tool::DispatchContext& prototype) {
    const auto id = call.tool_use_id;
    const auto name = call.name;
    ++shared->active_calls;
    ++shared->active_contexts[&prototype];
    asio::co_spawn(shared->executor,
                   run_call(state, shared, std::move(call), std::ref(prototype)),
                   asio::bind_cancellation_slot(
                       state->child_cancels[index].slot(),
                       [shared, state, index, context = &prototype, id, name](std::exception_ptr exception,
                                                                              core::Result<tool::Output> output) {
                         if (exception)
                           output = std::unexpected(core::Error::internal("tool dispatch failed unexpectedly"));
                         state->results[index] = ToolBatchResult{id, name, std::move(output)};
                         --shared->active_calls;
                         if (--shared->active_contexts[context] == 0)
                           shared->active_contexts.erase(context);
                         for (auto& weak : shared->idle_waiters)
                           if (auto waiter = weak.lock())
                             waiter->cancel();
                         shared->idle_waiters.clear();
                         // Report only after the dispatch frame and context are released.
                         static_cast<void>(state->completion.try_send(index));
                       }));
  }

  [[nodiscard]] static async::Awaitable<core::Result<tool::Output>>
  dispatch(tool::Registry& registry, const ToolBatchCall& call, tool::DispatchContext& context) {
    try {
      co_return co_await registry.dispatch(call.name, call.input_json, context);
    } catch (...) {
      // A thrown branch is not a completed Result in Asio's || race: translate
      // here so a failed effect cannot wait for the timeout or strand the queue.
    }
    const auto cancellation = co_await asio::this_coro::cancellation_state;
    co_return std::unexpected(cancellation.cancelled() != asio::cancellation_type::none
                                  ? parent_cancelled_error()
                                  : core::Error::internal("tool dispatch failed unexpectedly"));
  }

  [[nodiscard]] static async::Awaitable<core::Result<tool::Output>>
  run_call(std::shared_ptr<BatchState> state,
           std::shared_ptr<SharedState> shared,
           ToolBatchCall call,
           std::reference_wrapper<tool::DispatchContext> prototype_ref) {
    auto per_call_ctx = tool::DispatchContext::for_now(prototype_ref.get(), state->thread_approval_token_output);
    per_call_ctx.path_locks = &shared->locks;

    core::Result<tool::Output> output =
        std::unexpected(core::Error::internal("scheduler: race did not produce a result"));
    {
      using namespace asio::experimental::awaitable_operators;
      auto raced = co_await (dispatch(*shared->registry, call, per_call_ctx) ||
                             async::sleep_for(state->executor, state->options.per_call_timeout));
      if (auto* dispatched = std::get_if<core::Result<tool::Output>>(&raced); dispatched != nullptr) {
        output = std::move(*dispatched);
      } else if (auto* timer = std::get_if<core::Result<void>>(&raced); timer != nullptr) {
        if (!*timer) {
          // The timer was cancelled (parent cancellation) before it expired.
          output = std::unexpected(parent_cancelled_error());
        } else {
          // The timer expired naturally — per-call timeout wins.
          output = std::unexpected(timeout_error(call.name, state->options.per_call_timeout));
        }
      }
    }

    co_return output;
  }

  /// Drain pending completions until every call has reported. Used inside the
  /// phase-2 grace race in `run_batch`: when the grace timer wins, this
  /// coroutine's `receive()` is cancelled and it returns, leaving the indices
  /// it has not yet seen marked for `cancellation_lag` naming. `reported` and
  /// `completed` reference the awaiting `run_batch` frame, which stays alive
  /// across the race.
  [[nodiscard]] static async::Awaitable<void>
  drain_remaining(std::shared_ptr<BatchState> state, std::vector<bool>& reported, std::size_t& completed) {
    while (completed < state->total_calls) {
      auto next = co_await state->completion.receive();
      if (!next) {
        co_return;
      }
      reported[*next] = true;
      ++completed;
    }
  }

  // Best-effort diagnostics must not replace the parent cancellation result.
  [[nodiscard]] static async::Awaitable<core::Result<void>>
  record_cancellation_lag(tool::DispatchContext& prototype,
                          std::string_view tool_name,
                          std::chrono::milliseconds per_call_timeout) {
    permission::AuditEvent event;
    event.event_kind = "cancellation_lag";
    event.scope_key = prototype.scope_key;
    event.agent_key = prototype.agent_key;
    event.tool_name = std::string{tool_name};
    event.identity = prototype.identity;
    event.verdict = permission::Verdict::allow;
    event.outcome = permission::AuditOutcome::allow;
    event.reason = "cancellation_lag";
    event.parent_turn_id = prototype.parent_turn_id;
    event.metadata_json = cancellation_lag_metadata_json(per_call_timeout);
    co_return co_await prototype.audit.record(std::move(event));
  }

  std::shared_ptr<SharedState> state_;
};

ToolScheduler::ToolScheduler(asio::any_io_executor executor, tool::Registry& registry, ToolSchedulerOptions options)
    : impl_{std::make_unique<Impl>(std::move(executor), registry, options)} {}

ToolScheduler::~ToolScheduler() = default;

ToolScheduler::ToolScheduler(ToolScheduler&&) noexcept = default;

ToolScheduler& ToolScheduler::operator=(ToolScheduler&&) noexcept = default;

async::Awaitable<core::Result<std::vector<ToolBatchResult>>>
ToolScheduler::run_batch(std::vector<ToolBatchCall> batch, tool::DispatchContext& prototype) {
  return impl_->run_batch(std::move(batch), prototype);
}

async::Awaitable<core::Result<void>> ToolScheduler::wait_idle(const tool::DispatchContext* context) {
  return impl_->wait_idle(context);
}

}  // namespace orangutan::agent
