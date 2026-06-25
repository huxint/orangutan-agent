// include/oran/bootstrap/serve.hpp — long-lived service mode (`--serve`).
//
// The runtime-service owner (ROADMAP Dependency Frontier #2). The owner is a
// mode of the main binary, parallel to `--desktop`: `run_serve` starts
// `async::Runtime`, traps SIGINT/SIGTERM, co-spawns the long-lived service body,
// blocks until a signal arrives, then gracefully cancels it and tears the
// runtime down. Unlike `--desktop` it is not build-gated.
//
// The service body auto-starts the IO file-view cache watcher
// (`io::watch_read_text_file_ranged_cache`); when the loaded config carries
// `automation.cron.jobs[]`, it also runs the automation cron/triggered service
// loop (`automation::AutomationService::run` over a configured — or offline
// fake — provider route) plus the tool-scheduler idle-lock reaping tick
// (`agent::ToolScheduler::reap_idle_locks`); when the config carries buildable
// `channels[]`, it starts those adapters, pumps inbound messages through the
// `ChannelManager` fan-in, dispatches them through the routed agent bridge, and
// stops the adapters before returning. The automation jobs share one
// strand-driven `agent::ToolScheduler`, so the reaping concern bounds its
// per-path lock table while the jobs run. Building on `async::Runtime` plus a
// fine-grained `asio::cancellation_signal` (rather than the one-shot
// `SignalScope`/`io.stop()` drain) is deliberate so durable writes and in-flight
// agent turns are never blunt-dropped — see `signal_drain.hpp` for the
// anticipated evolution.

#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include <asio/any_io_executor.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/automation.hpp>
#include <oran/bootstrap/bootstrap.hpp>
#include <oran/channel/dispatch.hpp>
#include <oran/core/result.hpp>

namespace orangutan::agent {
class ToolScheduler;
}  // namespace orangutan::agent

namespace orangutan::bootstrap {

/// Inputs for the long-lived service coroutine. Kept separate from
/// `BootstrapOptions` so `serve_run` is drivable from tests with a plain
/// `asio::io_context` and an `asio::cancellation_signal`, without a real
/// process, signal, or config file.
struct ServeOptions {
  /// Directory the IO file-view watcher observes recursively. Empty (or
  /// `watch_enabled == false`) disables the watcher; the service then simply
  /// idles until cancelled.
  std::string watch_root{};
  bool watch_enabled{true};
};

/// The reusable service body. Runs the enabled concerns — in slice A the IO
/// file-view watcher — and idles until its bound cancellation slot fires, then
/// returns `Result<void>{}` (a graceful stop is not an error). A watcher that
/// fails to initialize (for example, inotify unavailable) is non-fatal: the
/// failure is reported once and the service keeps idling until cancelled.
///
/// Co-spawn it with `asio::bind_cancellation_slot(stop.slot(), ...)` and emit
/// `asio::cancellation_type::terminal` on `stop` to stop it; every await inside
/// is cancel-aware (C11).
[[nodiscard]] async::Awaitable<core::Result<void>> serve_run(asio::any_io_executor executor, ServeOptions options);

/// Tunables for the automation service concern under `--serve`.
struct ServeAutomationOptions {
  /// Idle gap between automation ticks once a tick finds no immediately-due
  /// work. Each tick fires any cron job due at the current UTC minute and
  /// drains buffered triggered work, so this bounds how promptly a newly-due
  /// job is noticed. A cancellation interrupts the wait immediately.
  std::chrono::steady_clock::duration poll_interval{std::chrono::seconds{1}};
  /// Per-tick cap on cron jobs scanned and executed.
  std::size_t cron_job_limit{100};
  /// Per-tick cap on buffered triggered jobs drained.
  std::size_t triggered_max_jobs{100};
};

/// The automation service concern. Drives `automation::AutomationService::run`
/// (a finite, caller-clocked cycle) in a cancel-aware poll loop until stopped,
/// then returns `Result<void>{}` (a graceful stop is not an error). Cron seeds
/// must already be applied to `service`'s repository: this loop only *executes*
/// due work, so it never rewrites stored `last_fired_at`. A non-cancellation
/// error from a tick (for example, a repository failure) is reported once and
/// the loop then idles until cancelled — the same degraded-but-alive posture as
/// the watcher. A handler failure is recorded as a run row by the service and
/// does not stop the loop.
///
/// Stopping: `stop_requested` (checked before and after each tick) is the
/// authoritative, guaranteed stop. Parent cancellation promptly interrupts the
/// idle wait between ticks, but a *firing* tick runs the automation service's
/// cancellation-disabled durable-write path, which can swallow a cancellation
/// that arrives mid-tick — so a long-lived owner must supply `stop_requested`
/// (tied to the same signal as the cancellation) for a guaranteed, prompt stop.
/// `run_serve` does exactly this.
///
/// Exposed (rather than file-local) so it can be driven from tests with a real
/// `AutomationRuntime` over a temp database and fake handlers — no provider,
/// process, or signal required.
[[nodiscard]] async::Awaitable<core::Result<void>> serve_automation(asio::any_io_executor executor,
                                                                    automation::AutomationService& service,
                                                                    automation::CronJobHandler cron_handler,
                                                                    automation::TriggeredJobHandler triggered_handler,
                                                                    ServeAutomationOptions options = {},
                                                                    std::function<bool()> stop_requested = {});

/// Tunables for the tool-scheduler idle-lock reaping concern under `--serve`.
struct ServeSchedulerReapOptions {
  /// Gap between reap ticks. Each tick drops lock-table entries idle longer
  /// than the scheduler's configured `idle_lock_ttl`, bounding the table for a
  /// long-lived shared scheduler (spec 0012 AC10). A cancellation interrupts
  /// the wait immediately. The default trails the 5-minute default TTL closely
  /// enough that a pathological per-path workload cannot grow unbounded.
  std::chrono::steady_clock::duration reap_interval{std::chrono::minutes{1}};
};

/// The tool-scheduler idle-lock reaping concern. Periodically calls
/// `scheduler.reap_idle_locks(now)` in a cancel-aware loop until stopped, then
/// returns `Result<void>{}` (a graceful stop is not an error). Reaping is an
/// in-memory, cancellation-safe synchronous call, so — unlike the automation
/// loop — a tick never swallows a parent cancellation; `stop_requested` is
/// still honored (checked before each tick) for callers that prefer a
/// predicate-driven stop, and `run_serve` supplies the same signal-tied one it
/// gives the automation loop.
///
/// `scheduler` must be the same `agent::ToolScheduler` the automation jobs
/// dispatch through, and `executor` must be the single strand both run on: its
/// per-path lock table is single-strand by contract, so reaping and dispatch
/// must not race across threads.
///
/// Exposed (rather than file-local) so it can be driven from tests against a
/// real `ToolScheduler` with a populated lock table — no provider, process, or
/// signal required.
[[nodiscard]] async::Awaitable<core::Result<void>> serve_scheduler_reaping(asio::any_io_executor executor,
                                                                           agent::ToolScheduler& scheduler,
                                                                           ServeSchedulerReapOptions options = {},
                                                                           std::function<bool()> stop_requested = {});

/// The channel ingress/dispatch concern — the first daemon owner of the channel
/// fan-in loop (`design-docs/channel-abstraction.md`: "That ownership lands with
/// the first daemon/dispatcher slice"). For every id in `channel_ids` it spawns
/// one cancel-aware *pump* coroutine that forwards that adapter's
/// `next_message()` results into the manager fan-in (`receive_one`), while a
/// single *dispatch* loop consumes the fan-in through `channel::dispatch_one`,
/// runs the routed agent `runner`, and replies via the owning adapter. The
/// adapters in `manager` must already be started (`start_all`).
///
/// A per-message dispatch failure (a malformed inbound, an agent-path error, or
/// a send failure) is reported and the loop continues — one bad message must not
/// kill the daemon. `dispatch_one` consumes the fan-in message before those
/// failures, so reporting-and-continuing cannot hot-spin on the same message.
///
/// Stopping: parent cancellation is the authoritative stop — the dispatcher and
/// every pump park on cancel-aware channel receives, so the shared signal
/// unblocks the dispatcher. `stop_requested` is a cooperative early-out checked
/// at the top of the dispatch loop. On either stop the concern marks the pumps
/// stopping, calls `manager.stop_all()` to wake adapter-owned receives, emits
/// each pump's cancellation, and drains them before returning, so no spawned
/// pump outlives this coroutine (each borrows `manager`). A null `runner` is
/// rejected up front.
///
/// Exposed (rather than file-local) so it can be driven from tests with a real
/// `ChannelManager` + `MockChannel` and a fake `ChannelPromptRunner` — no
/// provider, process, or signal required.
[[nodiscard]] async::Awaitable<core::Result<void>> serve_channels(asio::any_io_executor executor,
                                                                  channel::ChannelManager& manager,
                                                                  channel::ChannelPromptRunner runner,
                                                                  std::vector<std::string> channel_ids,
                                                                  std::function<bool()> stop_requested = {});

/// `orangutan --serve` entry. Loads config, starts `async::Runtime`, traps
/// SIGINT/SIGTERM on a runtime strand, co-spawns the service body, blocks the
/// calling thread until a signal arrives, then gracefully cancels the service
/// and stops the runtime. The service body always runs the IO file-view
/// watcher; when the config carries `automation.cron.jobs[]` it also opens
/// `<workspace>/.orangutan/automation.db`, applies those seeds once, and races
/// the automation loop and the tool-scheduler idle-lock reaping tick beside the
/// watcher (a configured `default` route drives a live provider, otherwise an
/// offline scripted fake keeps the loop usable; the automation jobs and the
/// reaping tick share one strand-driven `agent::ToolScheduler`).
/// On a trapped signal it returns a `cancelled` error carrying `signal`/`signum`
/// context, which `bootstrap::run` maps to the shell-conventional `128 + signum`
/// exit code (the same seam as `--audit-init`/`--trace`). Returns `0` if the
/// service ever stops without a signal.
[[nodiscard]] core::Result<int> run_serve(const BootstrapOptions& options);

}  // namespace orangutan::bootstrap
