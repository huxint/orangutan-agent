// src/oran-bootstrap/serve.cpp — long-lived service mode (`--serve`).
//
// See include/oran/bootstrap/serve.hpp for the design. This TU owns the
// lifecycle (start the runtime, trap signals, co-spawn the service body, block,
// graceful cancel, stop) and two concerns: the IO file-view cache watcher
// (always) and the automation cron/triggered service loop (when the loaded
// config carries `automation.cron.jobs[]`). The tool-scheduler idle-lock
// reaping tick lands later as another `serve_body` concern under the same
// lifecycle.

#include <oran/bootstrap/serve.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <optional>
#include <print>
#include <string>
#include <utility>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/signal_set.hpp>

#include <oran/agent/scheduler.hpp>
#include <oran/async.hpp>
#include <oran/bootstrap/automation_cron.hpp>
#include <oran/bootstrap/automation_prompt_runner.hpp>
#include <oran/bootstrap/prompt_runner.hpp>
#include <oran/bootstrap/provider_backend.hpp>
#include <oran/bootstrap/runtime_assembly.hpp>
#include <oran/bootstrap/signal_drain.hpp>
#include <oran/config.hpp>
#include <oran/core/error.hpp>
#include <oran/core/stop_reason.hpp>
#include <oran/core/time.hpp>
#include <oran/io.hpp>
#include <oran/provider.hpp>
#include <oran/provider/fake.hpp>
#include <oran/tool/builtins.hpp>
#include <oran/tool/registry.hpp>

namespace orangutan::bootstrap {
namespace {

using ::orangutan::core::Error;
using ::orangutan::core::Result;

/// Idle in a cancel-aware loop until the calling coroutine's cancellation slot
/// fires. `async::sleep_for` yields a `cancelled` result when the awaiting
/// coroutine is cancelled, which ends the wait; a natural timer expiry just
/// re-arms, so the service idles indefinitely without a hidden deadline.
async::Awaitable<void> wait_until_cancelled(asio::any_io_executor executor) {
  for (;;) {
    auto slept = co_await async::sleep_for(executor, std::chrono::hours{24});
    if (!slept) {
      co_return;  // cancelled (or timer error) — stop idling.
    }
  }
}

/// Scripted offline reply for automation when no provider route resolves, so
/// `orangutan --serve` still fires scheduled jobs (against a fake model) without
/// a configured route — the same offline posture as `--desktop`. A real route
/// drives the live provider through the identical prompt-runner bridge. Enough
/// turns are scripted that an hour of minute-cadence fires has material before
/// the fake reports "plan exhausted" (surfaced as a recorded failed run).
[[nodiscard]] std::vector<provider::ScriptedTurn> serve_offline_plan() {
  std::vector<provider::ScriptedTurn> plan;
  for (int turn = 0; turn < 64; ++turn) {
    plan.push_back(provider::ScriptedTurn{
        .response = std::nullopt,
        .deltas =
            {
                provider::TextDelta{.text = "Automation ran offline: no provider route is configured, so "},
                provider::TextDelta{.text = "this scheduled job produced a scripted reply. Add a route to "},
                provider::TextDelta{.text = "fire a real model."},
                provider::StreamEnd{.stop_reason = core::StopReason::end_turn,
                                    .usage = std::nullopt,
                                    .model_used = std::nullopt},
            },
        .error = std::nullopt,
        .latency = std::chrono::milliseconds{0},
    });
  }
  return plan;
}

[[nodiscard]] provider::Route serve_offline_route() {
  return provider::Route{
      .primary = provider::ModelTarget{.profile = "serve-offline",
                                       .model = "scripted-1",
                                       .protocol = provider::ProtocolKind::anthropic_messages,
                                       .thinking_budget = std::nullopt,
                                       .cache = std::nullopt},
      .fallbacks = {},
  };
}

/// The composed service body. A free coroutine (not a capture-by-reference
/// lambda) so its inputs are moved into the coroutine frame and cannot dangle
/// after the spawning full-expression. Opens automation persistence on the
/// runtime, applies the config cron seeds once, then races the watcher beside
/// the automation loop and the scheduler idle-lock reaping tick (over the
/// shared `scheduler`) under the caller's cancellation slot. A database that
/// cannot open (or seeds that cannot apply) is non-fatal: report once and serve
/// the watcher alone so a signal still stops cleanly.
async::Awaitable<Result<void>> serve_body(asio::any_io_executor executor,
                                          ServeOptions watch_options,
                                          bool automation_enabled,
                                          std::string automation_db,
                                          std::vector<automation::UpsertCronJobRequest> cron_seeds,
                                          automation::CronJobHandler cron_handler,
                                          automation::TriggeredJobHandler triggered_handler,
                                          ServeAutomationOptions automation_options,
                                          agent::ToolScheduler* scheduler,
                                          ServeSchedulerReapOptions reap_options,
                                          std::function<bool()> stop_requested) {
  if (!automation_enabled) {
    co_return co_await serve_run(executor, std::move(watch_options));
  }

  auto automation_runtime = co_await automation::AutomationRuntime::open(
      executor,
      automation::AutomationRuntimeOptions{.database_path = std::move(automation_db)});
  if (!automation_runtime) {
    std::println(stderr,
                 "orangutan: automation runtime unavailable, serving file-view watcher only: {}",
                 automation_runtime.error());
    co_return co_await serve_run(executor, std::move(watch_options));
  }

  if (auto seeded = co_await automation_runtime->apply_cron_job_seeds(std::move(cron_seeds)); !seeded) {
    std::println(stderr, "orangutan: automation cron seeds failed, serving file-view watcher only: {}", seeded.error());
    co_return co_await serve_run(executor, std::move(watch_options));
  }

  auto service = automation_runtime->automation_service();

  using namespace asio::experimental::awaitable_operators;
  // The automation jobs and the reaping tick share `scheduler`, so race all
  // three concerns under one cancellation slot. `scheduler` is non-null
  // whenever automation is enabled (run_serve builds it); the guard keeps the
  // body robust if a future caller enables automation without a shared
  // scheduler.
  if (scheduler != nullptr) {
    // Pass `stop_requested` by copy to both predicate consumers: the evaluation
    // order of `||` operands is unspecified, so moving into one while copying
    // into the other could hand the copy a moved-from (empty) function.
    co_await (serve_run(executor, std::move(watch_options)) ||
              serve_automation(executor,
                               service,
                               std::move(cron_handler),
                               std::move(triggered_handler),
                               automation_options,
                               stop_requested) ||
              serve_scheduler_reaping(executor, *scheduler, reap_options, stop_requested));
  } else {
    co_await (serve_run(executor, std::move(watch_options)) || serve_automation(executor,
                                                                                service,
                                                                                std::move(cron_handler),
                                                                                std::move(triggered_handler),
                                                                                automation_options,
                                                                                std::move(stop_requested)));
  }
  co_return Result<void>{};
}

}  // namespace

async::Awaitable<Result<void>> serve_run(asio::any_io_executor executor, ServeOptions options) {
  if (options.watch_enabled && !options.watch_root.empty()) {
    // The watcher runs until cancelled (`max_events == 0`); on cancellation it
    // returns a `cancelled` result, which is a graceful stop here.
    auto watched = co_await io::watch_read_text_file_ranged_cache(
        executor,
        options.watch_root,
        io::ReadTextFileWatchOptions{.recursive = true, .max_events = 0});
    if (!watched && watched.error().kind() != core::ErrorKind::cancelled) {
      // The watcher could not start (for example, inotify unavailable). Report
      // it once and keep serving so a later signal still stops cleanly.
      std::println(stderr, "orangutan: file-view watcher unavailable: {}", watched.error());
      co_await wait_until_cancelled(executor);
    }
    co_return Result<void>{};
  }

  co_await wait_until_cancelled(executor);
  co_return Result<void>{};
}

async::Awaitable<Result<void>> serve_automation(asio::any_io_executor executor,
                                                automation::AutomationService& service,
                                                automation::CronJobHandler cron_handler,
                                                automation::TriggeredJobHandler triggered_handler,
                                                ServeAutomationOptions options,
                                                std::function<bool()> stop_requested) {
  for (;;) {
    if (stop_requested && stop_requested()) {
      co_return Result<void>{};  // cooperative stop before starting a new tick.
    }

    // One bounded cycle clocked at the current UTC minute: drain buffered
    // triggered work and execute any cron job due now. `max_total_wait == 0`
    // keeps the cycle from sleeping internally — this loop owns the cadence —
    // and no cron seeds are passed, so stored `last_fired_at` is never reset.
    auto ran = co_await service.run(automation::AutomationServiceRunRequest{
        .cycle =
            automation::AutomationServiceCycleRequest{
                .now = core::time::now_utc(),
                .max_total_wait = std::chrono::steady_clock::duration::zero(),
                .max_iterations = 1,
                .cron_job_limit = options.cron_job_limit,
                .cron_handler = cron_handler,
                .stop_requested = stop_requested,
                .triggered_handler = triggered_handler,
                .triggered_max_jobs = options.triggered_max_jobs,
            },
        .max_iterations = 1,
        .retry_wait = std::chrono::steady_clock::duration::zero(),
        .stop_requested = stop_requested,
    });
    if (!ran) {
      if (ran.error().kind() == core::ErrorKind::cancelled) {
        co_return Result<void>{};  // a signal interrupted the tick — graceful stop.
      }
      // A repository/database failure is non-fatal to the service: report once
      // and idle until cancelled, the same degraded-but-alive posture as the
      // watcher. (Handler failures are recorded as run rows by the service and
      // surface as a stop reason, not an error, so they do not land here.)
      std::println(stderr, "orangutan: automation tick failed: {}", ran.error());
      co_await wait_until_cancelled(executor);
      co_return Result<void>{};
    }

    // A firing tick runs the automation service's cancellation-disabled durable
    // write/lease-release path (oran-automation/service.cpp), which can swallow
    // a parent cancellation that arrived mid-tick. Re-check the cooperative stop
    // here so a signal taken during a fire stops promptly instead of waiting out
    // a full poll interval — `stop_requested` is the authoritative stop and
    // `run_serve` always supplies it.
    if (stop_requested && stop_requested()) {
      co_return Result<void>{};
    }

    auto slept = co_await async::sleep_for(executor, options.poll_interval);
    if (!slept) {
      co_return Result<void>{};  // cancelled during the idle gap — graceful stop.
    }
  }
}

async::Awaitable<Result<void>> serve_scheduler_reaping(asio::any_io_executor executor,
                                                       agent::ToolScheduler& scheduler,
                                                       ServeSchedulerReapOptions options,
                                                       std::function<bool()> stop_requested) {
  for (;;) {
    if (stop_requested && stop_requested()) {
      co_return Result<void>{};  // cooperative stop before the next tick.
    }

    auto slept = co_await async::sleep_for(executor, options.reap_interval);
    if (!slept) {
      co_return Result<void>{};  // cancelled during the idle gap — graceful stop.
    }

    // Reaping is a synchronous in-memory sweep of the per-path lock table; it
    // does not await or disable cancellation, so — unlike the automation tick —
    // it can never swallow a parent cancellation. The clock matches the lock
    // table's acquire/release stamp (`core::time::now_utc()`), so idle ages are
    // compared on the same basis. The returned reap count is intentionally
    // dropped: pre-`oran-log` there is no structured sink, and `lock_stats()`
    // already exposes the cumulative `reaped_entries` for `--explain-rules`.
    static_cast<void>(scheduler.reap_idle_locks(core::time::now_utc()));
  }
}

Result<int> run_serve(const BootstrapOptions& options) {
  auto loaded = load_config(options);
  if (!loaded) {
    return std::unexpected(std::move(loaded).error());
  }
  const auto& cfg = loaded->value;

  // Config-authored cron seeds gate the automation concern: with none, `--serve`
  // is exactly the watcher (no provider, no `automation.db`, CI-identical).
  auto cron_seeds = cron_jobs_from(cfg);
  if (!cron_seeds) {
    return std::unexpected(std::move(cron_seeds).error());
  }
  const bool automation_enabled = !cron_seeds->empty();
  const auto cron_job_count = cron_seeds->size();

  auto runtime = async::Runtime{async::RuntimeConfig{
      .io_workers = static_cast<std::size_t>(std::max<std::int64_t>(1, cfg.runtime().workers)),
      .cpu_workers = 1,
  }};

  // The SIGINT/SIGTERM handler and the service coroutine share one strand, so
  // the handler's `stop.emit(...)` is serialized with the slot it cancels —
  // safe regardless of io-worker count, since an `asio::cancellation_signal` is
  // not itself thread-safe.
  auto strand = runtime.make_strand();
  asio::cancellation_signal stop;
  std::atomic<int> caught_signum{0};
  std::promise<Result<void>> service_done;
  auto service_future = service_done.get_future();

  asio::signal_set signals{strand, SIGINT, SIGTERM};
  signals.async_wait([&caught_signum, &stop](const asio::error_code& ec, int signum) {
    if (!ec) {
      caught_signum.store(signum, std::memory_order_release);
      stop.emit(asio::cancellation_type::terminal);
    }
  });

  const auto watch_root = options.workspace;

  // Automation wiring lives on this stack so the runtime assembly, provider
  // backend, and the shared tool registry + scheduler (all borrowed by the
  // per-job prompt runner) outlive the service coroutine — the coroutine, and
  // the `AutomationService` it opens, completes before `service_future.get()`
  // returns, i.e. before these are destroyed.
  std::optional<RuntimeAssembly> assembly;
  std::optional<HttpProviderBackend> live_backend;
  std::optional<provider::FakeProvider> offline_provider;
  std::optional<tool::Registry> shared_registry;
  std::optional<agent::ToolScheduler> shared_scheduler;
  automation::CronJobHandler cron_handler;
  automation::TriggeredJobHandler triggered_handler;
  std::string automation_db;
  bool automation_live = false;

  if (automation_enabled) {
    const bool has_route = !cfg.routes().empty();

    auto assembly_options = RuntimeAssemblyOptions{};
    assembly_options.workspace_options = tool::WorkspaceOptions{
        .extra_read_roots = cfg.permissions().workspace.extra_read_roots,
        .extra_write_roots = cfg.permissions().workspace.extra_write_roots,
    };
    assembly_options.trace_enabled = cfg.trace().enabled;
    assembly_options.hook_blocking_timeout = std::chrono::milliseconds{cfg.hooks().timeout_ms};
    assembly_options.session_memory_enabled = has_route;
    assembly_options.longterm_memory_enabled = has_route;
    auto built = RuntimeAssembly::build(options.workspace, runtime.executor(), std::move(assembly_options));
    if (!built) {
      return std::unexpected(std::move(built).error());
    }
    assembly.emplace(std::move(*built));

    // One registry + scheduler shared across every per-job runner. Driving them
    // on `strand` (not the multi-worker `runtime.executor()`) honors the
    // scheduler's single-strand lock-table contract and lets the reaping tick
    // sweep that table without racing in-flight dispatch.
    shared_registry.emplace();
    if (auto registered = tool::register_builtins(*shared_registry); !registered) {
      return std::unexpected(std::move(registered).error());
    }
    auto scheduler_opts = scheduler_options_from(cfg);
    if (!scheduler_opts) {
      return std::unexpected(std::move(scheduler_opts).error());
    }
    shared_scheduler.emplace(strand, *shared_registry, *scheduler_opts);

    provider::System* system = nullptr;
    provider::Route route{};
    if (has_route) {
      auto backend =
          HttpProviderBackend::build(cfg,
                                     HttpProviderBackendOptions{
                                         .blocking_executor = runtime.cpu_executor(),
                                         .request_timeout = std::chrono::milliseconds{cfg.runtime().request_timeout_ms},
                                         .route_name = "default",
                                     });
      if (!backend) {
        return std::unexpected(std::move(backend).error());
      }
      live_backend.emplace(std::move(*backend));
      system = &live_backend->system();
      route = live_backend->route();
      automation_live = true;
    } else {
      offline_provider.emplace(serve_offline_plan());
      system = &*offline_provider;
      route = serve_offline_route();
    }

    auto prompt_runner = make_automation_agent_prompt_runner(AutomationAgentPromptRunnerOptions{
        .executor = strand,
        .assembly = &*assembly,
        .config = &cfg,
        .provider = system,
        .route = std::move(route),
        .max_tokens = 1024,
        .registry = &*shared_registry,
        .scheduler = &*shared_scheduler,
    });
    if (!prompt_runner) {
      return std::unexpected(std::move(prompt_runner).error());
    }
    cron_handler = automation::make_cron_prompt_handler(*prompt_runner);
    triggered_handler = automation::make_triggered_prompt_handler(std::move(*prompt_runner));

    automation_db = (std::filesystem::path{options.workspace} / ".orangutan" / "automation.db").string();
  }

  auto stop_predicate = [&caught_signum]() -> bool {
    return caught_signum.load(std::memory_order_acquire) != 0;
  };

  // Queue the signal wait and the coroutine first, then start the workers, so
  // the only thread that touches `stop` after launch is the strand. `serve_body`
  // copies `automation_db` (the banner below still reads the local).
  asio::co_spawn(strand,
                 serve_body(strand,
                            ServeOptions{.watch_root = watch_root, .watch_enabled = true},
                            automation_enabled,
                            automation_db,
                            std::move(*cron_seeds),
                            std::move(cron_handler),
                            std::move(triggered_handler),
                            ServeAutomationOptions{},
                            automation_enabled ? &*shared_scheduler : nullptr,
                            ServeSchedulerReapOptions{},
                            stop_predicate),
                 asio::bind_cancellation_slot(stop.slot(), [&service_done](std::exception_ptr ep, Result<void> result) {
                   if (ep) {
                     service_done.set_value(
                         std::unexpected(Error::internal("service coroutine terminated by exception")));
                     return;
                   }
                   service_done.set_value(std::move(result));
                 }));

  if (auto started = runtime.start(); !started) {
    return std::unexpected(std::move(started).error());
  }

  std::println("orangutan: service mode started");
  std::println("  io watcher: {} (recursive)", watch_root);
  if (automation_enabled) {
    std::println("  automation: {} ({} cron job(s), {} provider)",
                 automation_db,
                 cron_job_count,
                 automation_live ? "live" : "offline");
    std::println("  scheduler:  idle-lock reaping every {}s",
                 std::chrono::duration_cast<std::chrono::seconds>(ServeSchedulerReapOptions{}.reap_interval).count());
  }
  std::println("  stop with Ctrl-C (SIGINT) or SIGTERM");

  auto result = service_future.get();  // blocks until a signal stops the service
  runtime.stop();

  if (const auto signum = caught_signum.load(std::memory_order_acquire); signum != 0) {
    return std::unexpected(
        Error::cancelled().with("signal", std::string{signal_name(signum)}).with("signum", std::to_string(signum)));
  }
  if (!result) {
    return std::unexpected(std::move(result).error());
  }
  return 0;
}

}  // namespace orangutan::bootstrap
