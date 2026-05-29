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
#include <oran/permission/audit.hpp>
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

/// AC5 cancellation budget: once the parent token is cancelled, `run_batch`
/// waits at most this long for in-flight calls to wind down before returning
/// `parent_cancelled`. Cancel-aware tools observe the emitted cancellation and
/// resolve well inside this window; a tool that never polls its cancellation
/// slot misses it and is named in a `cancellation_lag` audit row.
constexpr std::chrono::milliseconds kCancellationGrace{100};

/// Audit `metadata_json` for a `cancellation_lag` row. `error_kind` is the
/// spec-0012 AC5 marker; `cancellation_grace_ms` records the window the tool
/// missed and `per_call_timeout_ms` the bound that still backstops it.
[[nodiscard]] std::string cancellation_lag_metadata_json(std::chrono::milliseconds per_call_timeout) {
  nlohmann::json metadata;
  metadata["error_kind"] = "cancellation_lag";
  metadata["cancellation_grace_ms"] = kCancellationGrace.count();
  metadata["per_call_timeout_ms"] = per_call_timeout.count();
  return metadata.dump();
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

    std::vector<std::string> names;
    names.reserve(batch.size());
    for (std::size_t i = 0; i < batch.size(); ++i) {
      names.push_back(batch[i].name);
      asio::co_spawn(executor_,
                     run_call(state, registry_, &locks_, std::move(batch[i]), i, std::ref(prototype)),
                     asio::bind_cancellation_slot(state->child_cancels[i].slot(), asio::detached));
    }

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
    for (auto& signal : state->child_cancels) {
      signal.emit(asio::cancellation_type::all);
    }
    co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());

    // Wait out the AC5 grace window for the remaining calls. A cancel-aware
    // tool resolves almost at once; a tool that ignores its cancellation slot
    // keeps `run_call` suspended in its `dispatch || timeout` race — that race
    // cannot resolve until the handler returns, because asio cancellation is
    // cooperative — so we race the drain against `kCancellationGrace` and stop
    // awaiting at the deadline instead of stalling the whole batch.
    if (completed < state->total_calls) {
      using namespace asio::experimental::awaitable_operators;
      co_await (drain_remaining(state, reported, completed) || async::sleep_for(executor_, kCancellationGrace));
    }

    // Name every call that missed the grace window. A laggard keeps running
    // (it holds the shared `BatchState` alive) and will wind down on its own;
    // the audit row records `error_kind=cancellation_lag` against the tool so
    // the offending handler is identifiable.
    for (std::size_t i = 0; i < state->total_calls; ++i) {
      if (!reported[i]) {
        [[maybe_unused]] auto recorded = co_await record_cancellation_lag(prototype, names[i]);
      }
    }

    co_return std::unexpected(parent_cancelled_error());
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

  /// Record one `cancellation_lag` audit row naming a tool that did not wind
  /// down within `kCancellationGrace` after a parent cancellation. The row
  /// reuses the prototype's scope / agent / identity / parent-turn correlation
  /// so `--explain-rules`-style and `--trace` consumers see it beside the
  /// call's permission-decision row. Best-effort: a failed record is discarded
  /// by the caller and never masks the `parent_cancelled` result.
  [[nodiscard]] async::Awaitable<core::Result<void>> record_cancellation_lag(tool::DispatchContext& prototype,
                                                                             std::string_view tool_name) {
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
    event.metadata_json = cancellation_lag_metadata_json(options_.per_call_timeout);
    co_return co_await prototype.audit.record(std::move(event));
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
