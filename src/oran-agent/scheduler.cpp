// src/oran-agent/scheduler.cpp — bounded-parallel scheduler.

#include <oran/agent/scheduler.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>
#include <exception>
#include <expected>
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
#include <asio/detached.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/this_coro.hpp>

#include <nlohmann/json.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/async/channel.hpp>
#include <oran/async/sleep.hpp>
#include <oran/core/capability.hpp>
#include <oran/core/error.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/tool/output.hpp>
#include <oran/tool/registry.hpp>
#include <oran/tool/workspace.hpp>

#include "_impl/path_lock_table.hpp"

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

/// Per-call dispatch context built from the prototype + per-call mutable
/// state. Each spawned dispatch builds its own; the prototype's references
/// rebind to the same long-lived services. `now` is refreshed per call so
/// broker / approval TTL checks see the real clock; `registry` and
/// `resolved_path` are reset because `Registry::dispatch` writes them itself.
[[nodiscard]] tool::DispatchContext make_per_call_context(tool::DispatchContext& prototype) {
  return tool::DispatchContext{
      .executor = prototype.executor,
      .mode = prototype.mode,
      .rules = prototype.rules,
      .audit = prototype.audit,
      .approval_broker = prototype.approval_broker,
      .approval_token = prototype.approval_token,
      .approval_token_output = nullptr,
      .now = core::time::now_utc(),
      .bus = prototype.bus,
      .registry = nullptr,
      .workspace = prototype.workspace,
      .resolved_path = std::nullopt,
      .output_caps = prototype.output_caps,
      .parent_turn_id = prototype.parent_turn_id,
      .scope_key = prototype.scope_key,
      .agent_key = prototype.agent_key,
      .identity = prototype.identity,
  };
}

/// Classify a tool's lock requirement from its declared capabilities. The
/// classification is deterministic per `core::ToolDef`: a tool that touches
/// the filesystem via `Capability::write_file` / `edit_file` / `delete_path`
/// takes an exclusive per-path lock, a tool that reads via
/// `Capability::read_file` / `list_directory` takes a shared per-path lock,
/// and everything else (e.g., `tool.search`, memory tools, future shell /
/// agent.spawn workloads) skips path locking. The spec calls out
/// `shell.exec` / `agent.spawn` / `tool.runtime_loader` as globally
/// serialised in a future revision — they keep no v1 lock until that ships.
[[nodiscard]] std::optional<detail::PathLockMode>
classify_lock_mode(const std::vector<core::Capability>& capabilities) {
  bool wants_write = false;
  bool wants_read = false;
  for (auto cap : capabilities) {
    switch (cap) {
      case core::Capability::write_file:
      case core::Capability::edit_file:
      case core::Capability::delete_path:
        wants_write = true;
        break;
      case core::Capability::read_file:
      case core::Capability::list_directory:
        wants_read = true;
        break;
      default:
        break;
    }
  }
  if (wants_write) {
    return detail::PathLockMode::exclusive;
  }
  if (wants_read) {
    return detail::PathLockMode::shared;
  }
  return std::nullopt;
}

/// Resolve the lock key for a call by extracting `path` from the JSON input
/// and routing it through the prototype workspace's intent-matching resolver.
/// Returns `std::nullopt` for the cases where lock acquisition should be
/// skipped (no workspace, no `path` field, resolution failure, malformed
/// JSON). A skipped lock falls through to bounded-parallelism only; the
/// underlying registry dispatch still re-runs full resolution and returns the
/// same error the lock-keyed path would have surfaced.
[[nodiscard]] std::optional<std::string> derive_lock_key(tool::Workspace* workspace,
                                                         std::string_view input_json,
                                                         detail::PathLockMode mode,
                                                         const std::vector<core::Capability>& capabilities) {
  if (workspace == nullptr) {
    return std::nullopt;
  }

  nlohmann::json parsed;
  try {
    parsed = nlohmann::json::parse(input_json);
  } catch (const nlohmann::json::parse_error&) {
    return std::nullopt;
  } catch (const std::exception&) {
    return std::nullopt;
  }
  if (!parsed.is_object()) {
    return std::nullopt;
  }
  auto path_it = parsed.find("path");
  if (path_it == parsed.end() || !path_it->is_string()) {
    return std::nullopt;
  }
  const auto raw_path = path_it->get<std::string>();

  core::Result<tool::ResolvedPath> resolved =
      std::unexpected(core::Error::internal("derive_lock_key: unreachable resolver path"));
  if (mode == detail::PathLockMode::shared) {
    // Shared lock covers both `file.read` and the listing intents
    // (`file.search`, `directory.list`). The workspace's resolve_list shape
    // is permissive enough to accept both filenames and directory targets,
    // so use it for the listing capability.
    const bool list_intent = std::ranges::find(capabilities, core::Capability::list_directory) != capabilities.end();
    resolved = list_intent ? workspace->resolve_list(raw_path) : workspace->resolve_read(raw_path);
  } else {
    // Exclusive: delete vs. write/edit. delete_path implies `file.delete`;
    // otherwise resolve through `resolve_write` (covers `file.write` and
    // `file.edit`, both of which dispatch own re-resolution internally).
    const bool delete_intent = std::ranges::find(capabilities, core::Capability::delete_path) != capabilities.end();
    resolved =
        delete_intent ? workspace->resolve_delete(raw_path) : workspace->resolve_write(raw_path, tool::WriteIntent{});
  }

  if (!resolved.has_value()) {
    return std::nullopt;
  }
  return std::move(resolved->absolute_path);
}

struct BatchState {
  asio::any_io_executor executor;
  ToolSchedulerOptions options;
  std::size_t total_calls;
  std::vector<std::optional<ToolBatchResult>> results;
  async::Channel<std::monostate> semaphore;
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
      : executor{std::move(exec)}, options{opts}, total_calls{n}, results(n),
        semaphore{executor, opts.max_parallel_tools}, completion{executor, n} {
    for (std::size_t i = 0; i < n; ++i) {
      child_cancels.emplace_back();
    }
  }
};

}  // namespace

class ToolScheduler::Impl {
public:
  Impl(asio::any_io_executor executor, tool::Registry& registry, ToolSchedulerOptions options)
      : executor_{std::move(executor)}, registry_{&registry}, options_{options},
        locks_{detail::PathLockTableOptions{.idle_ttl = options.idle_lock_ttl}} {}

  [[nodiscard]] const ToolSchedulerOptions& options() const noexcept {
    return options_;
  }

  [[nodiscard]] ToolSchedulerLockStats lock_stats() const noexcept {
    const auto inner = locks_.stats();
    return ToolSchedulerLockStats{
        .shared_acquires = inner.shared_acquires,
        .exclusive_acquires = inner.exclusive_acquires,
        .contended_acquires = inner.contended_acquires,
        .cancelled_acquires = inner.cancelled_acquires,
        .reaped_entries = inner.reaped_entries,
        .current_entries = inner.current_entries,
        .peak_entries = inner.peak_entries,
    };
  }

  std::size_t reap_idle_locks(core::Time now) {
    return locks_.reap(now);
  }

  [[nodiscard]] async::Awaitable<core::Result<std::vector<ToolBatchResult>>>
  run_batch(std::vector<ToolBatchCall> batch, tool::DispatchContext& prototype) {
    if (batch.empty()) {
      co_return std::vector<ToolBatchResult>{};
    }

    if (auto cancel = co_await asio::this_coro::cancellation_state;
        cancel.cancelled() != asio::cancellation_type::none) {
      co_return std::unexpected(parent_cancelled_error());
    }

    auto state = std::make_shared<BatchState>(executor_, batch.size(), options_);

    // Fill the channel-as-semaphore with one permit per concurrency slot. The
    // `try_send` calls cannot fail because we just constructed the channel
    // with capacity == max_parallel_tools; they are documented to discard
    // their result on the happy path.
    for (std::size_t i = 0; i < options_.max_parallel_tools; ++i) {
      [[maybe_unused]] auto permit = state->semaphore.try_send(std::monostate{});
    }

    for (std::size_t i = 0; i < batch.size(); ++i) {
      asio::co_spawn(executor_,
                     run_call(state, registry_, &locks_, std::move(batch[i]), i, std::ref(prototype)),
                     asio::bind_cancellation_slot(state->child_cancels[i].slot(), asio::detached));
    }

    bool parent_cancelled = false;
    std::size_t completed = 0;
    while (completed < state->total_calls) {
      auto next = co_await state->completion.receive();
      if (!next) {
        // Parent cancellation reached our `receive()`. Emit on every child
        // signal so each in-flight dispatch sees its own cancellation
        // contract, then drain remaining completions with the parent slot
        // filtered. Each spawned coroutine still sends exactly one completion
        // before exiting, so the drain is finite.
        parent_cancelled = true;
        for (auto& signal : state->child_cancels) {
          signal.emit(asio::cancellation_type::all);
        }
        co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
        continue;
      }
      ++completed;
    }

    if (parent_cancelled) {
      co_return std::unexpected(parent_cancelled_error());
    }

    std::vector<ToolBatchResult> ordered;
    ordered.reserve(state->total_calls);
    for (auto& slot : state->results) {
      ordered.emplace_back(std::move(*slot));
    }
    co_return ordered;
  }

private:
  [[nodiscard]] static async::Awaitable<void> run_call(std::shared_ptr<BatchState> state,
                                                       tool::Registry* registry,
                                                       detail::PathLockTable* locks,
                                                       ToolBatchCall call,
                                                       std::size_t index,
                                                       std::reference_wrapper<tool::DispatchContext> prototype_ref) {
    auto acquired = co_await state->semaphore.receive();
    if (!acquired) {
      state->results[index] = ToolBatchResult{
          .tool_use_id = std::move(call.tool_use_id),
          .name = std::move(call.name),
          .output = std::unexpected(parent_cancelled_error()),
      };
      [[maybe_unused]] auto sent = co_await state->completion.send(index);
      co_return;
    }

    // Per-canonical-path lock acquisition. Classification is from the tool
    // def's required capabilities; the lock key is the workspace-resolved
    // absolute path. Tools without a lock class, or calls without a resolvable
    // path (no workspace, malformed input, or a path that fails workspace
    // resolution), skip the lock — the registry dispatch path still re-runs
    // resolution and produces the same end-state error as it would have
    // without the scheduler.
    detail::PathLockGuard lock_guard{};
    auto& prototype = prototype_ref.get();
    if (const core::ToolDef* def = registry->find(call.name); def != nullptr) {
      if (auto mode = classify_lock_mode(def->required_capabilities); mode.has_value()) {
        if (auto key = derive_lock_key(prototype.workspace, call.input_json, *mode, def->required_capabilities);
            key.has_value()) {
          auto acquired_lock = co_await locks->acquire(state->executor, *std::move(key), *mode, core::time::now_utc());
          if (!acquired_lock) {
            state->results[index] = ToolBatchResult{
                .tool_use_id = std::move(call.tool_use_id),
                .name = std::move(call.name),
                .output = std::unexpected(parent_cancelled_error()),
            };
            [[maybe_unused]] auto release = state->semaphore.try_send(std::monostate{});
            co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
            [[maybe_unused]] auto sent = co_await state->completion.send(index);
            co_return;
          }
          lock_guard = std::move(*acquired_lock);
        }
      }
    }

    auto per_call_ctx = make_per_call_context(prototype);

    core::Result<tool::Output> output =
        std::unexpected(core::Error::internal("scheduler: race did not produce a result"));
    {
      using namespace asio::experimental::awaitable_operators;
      auto raced = co_await (registry->dispatch(call.name, call.input_json, per_call_ctx) ||
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

    // Release the path lock before signalling completion so subsequent calls
    // see the lock state we leave behind. The guard's destructor would catch
    // this too, but doing it explicitly clarifies the release order.
    lock_guard = detail::PathLockGuard{};

    // Release the semaphore slot. Capacity == max_parallel_tools and we hold
    // exactly one permit, so `try_send` always succeeds here.
    [[maybe_unused]] auto release = state->semaphore.try_send(std::monostate{});

    state->results[index] = ToolBatchResult{
        .tool_use_id = std::move(call.tool_use_id),
        .name = std::move(call.name),
        .output = std::move(output),
    };

    // The completion channel was sized to total_calls, so this send cannot
    // back-pressure. Filter cancellation on the spawned coroutine so the
    // parent-emitted cancel does not turn this final signal into a no-op.
    co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
    [[maybe_unused]] auto sent = co_await state->completion.send(index);
  }

  asio::any_io_executor executor_;
  tool::Registry* registry_;
  ToolSchedulerOptions options_;
  detail::PathLockTable locks_;
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

const ToolSchedulerOptions& ToolScheduler::options() const noexcept {
  return impl_->options();
}

ToolSchedulerLockStats ToolScheduler::lock_stats() const noexcept {
  return impl_->lock_stats();
}

std::size_t ToolScheduler::reap_idle_locks(core::Time now) {
  return impl_->reap_idle_locks(now);
}

}  // namespace orangutan::agent
