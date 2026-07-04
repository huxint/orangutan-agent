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
// `automation.cron.jobs[]` or `automation.triggered.jobs[]`, it also runs the
// automation cron/triggered service loop (`automation::AutomationService::run`
// over a configured — or offline fake — provider route) plus the
// tool-scheduler idle-lock reaping tick
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
#include <cstdint>
#include <functional>
#include <optional>
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

/// Tunables for the HTTP webhook listener concern under `--serve`.
struct ServeWebhookOptions {
  /// Local address to bind. The first listener slice accepts numeric addresses
  /// only (for example `127.0.0.1` or `::1`) and intentionally avoids DNS.
  std::string bind_host{"127.0.0.1"};
  /// TCP port to bind. `0` is accepted for tests/embedders that want an
  /// ephemeral port; `run_serve` prints the bound endpoint after startup.
  std::uint16_t port{8787};
  /// Path prefix for `POST` requests. The webhook id is the single path segment
  /// after this prefix; `/automation/webhooks/ci` maps to `webhook:ci`.
  std::string path_prefix{"/automation/webhooks/"};
  /// Maximum accepted request body size. The listener reads only
  /// `Content-Length` bodies and rejects larger payloads before queueing.
  std::size_t max_payload_bytes{256 * 1024};
  /// Maximum triggered descriptors matched per webhook request.
  std::size_t job_limit{100};
  /// Optional observer for the actual port after bind. Useful when `port == 0`.
  std::function<void(std::uint16_t)> bound_observer{};
};

/// The HTTP webhook listener concern. It owns a small localhost HTTP intake
/// loop for `POST <path_prefix><webhook-id>` requests, validates a
/// `Content-Length` body under `max_payload_bytes`, and feeds the existing
/// `automation::WebhookProducer`. It deliberately does not expose a generic
/// HTTP router or support chunked transfer; this is the narrow external trigger
/// binding for automation. Each request is handled in its own coroutine and
/// the listener stops gracefully on parent cancellation or `stop_requested`.
[[nodiscard]] async::Awaitable<core::Result<void>> serve_webhooks(asio::any_io_executor executor,
                                                                  automation::AutomationService& service,
                                                                  ServeWebhookOptions options = {},
                                                                  std::function<bool()> stop_requested = {});

/// Structured snapshot for the channel conversation-worker table under
/// `--serve`. The counters are monotonic for one `serve_channels(...)` run
/// except `active_workers`, which is the current worker-table size.
struct ServeChannelWorkerMetrics {
  std::size_t active_workers{};
  std::size_t max_active_workers{};
  std::uint64_t workers_created{};
  std::uint64_t workers_completed{};
  std::uint64_t workers_evicted_idle{};
  std::uint64_t messages_enqueued{};
  std::uint64_t replies_sent{};
  std::uint64_t message_timeouts{};
  std::uint64_t dispatch_failures{};
  std::uint64_t enqueue_failures{};

  friend bool operator==(const ServeChannelWorkerMetrics&, const ServeChannelWorkerMetrics&) = default;
};

/// Options for the default channel worker metrics log sink. `emit_line`, when
/// set, receives already-formatted one-line status records; otherwise the sink
/// writes each record to stderr. Intended for `run_serve` plus tests/embedders
/// that want the same formatting without a process-level stderr dependency.
struct ServeChannelMetricsLogSinkOptions {
  std::function<void(std::string)> emit_line{};
};

/// Format one channel worker metrics snapshot as the operator-facing log line
/// emitted by `ServeChannelMetricsLogSink`.
[[nodiscard]] std::string format_serve_channel_worker_metrics(const ServeChannelWorkerMetrics& snapshot);

/// Deduplicating metrics observer for the channel concern under `--serve`.
/// Repeated identical snapshots are suppressed; changed snapshots are emitted
/// through `options.emit_line` or stderr when no callback is supplied.
class ServeChannelMetricsLogSink {
public:
  explicit ServeChannelMetricsLogSink(ServeChannelMetricsLogSinkOptions options = {});

  void operator()(const ServeChannelWorkerMetrics& snapshot);

private:
  ServeChannelMetricsLogSinkOptions options_{};
  std::optional<ServeChannelWorkerMetrics> last_snapshot_{};
};

/// Tunables for the channel ingress/dispatch concern under `--serve`.
struct ServeChannelOptions {
  /// Capacity of each per-channel+conversation queue. This bounds how far one
  /// conversation can backlog while preserving in-order dispatch.
  std::size_t conversation_queue_capacity{64};
  /// Idle gap after which an empty per-channel+conversation worker exits and is
  /// erased from the dispatcher table. A later message for the same key creates
  /// a fresh worker; this bounds long-lived services by active conversations
  /// instead of all historical conversations.
  std::chrono::steady_clock::duration conversation_idle_ttl{std::chrono::minutes{5}};
  /// Optional per-message deadline for the routed agent/reply send attempt.
  /// When set, the current attempt is cancelled on expiry and the worker sends
  /// a short still-working reply for that inbound message. A later durable
  /// rejoin path is not implemented yet.
  std::optional<std::chrono::steady_clock::duration> message_deadline{};
  /// Optional observer for worker-table metrics. Called synchronously on the
  /// dispatcher executor after worker creation/erasure, message enqueue, or a
  /// worker progress wake. The callback must be cheap and non-blocking.
  std::function<void(const ServeChannelWorkerMetrics&)> metrics_observer{};
};

/// The channel ingress/dispatch concern — the first daemon owner of the channel
/// fan-in loop (`design-docs/channel-abstraction.md`: "That ownership lands with
/// the first daemon/dispatcher slice"). For every id in `channel_ids` it spawns
/// one cancel-aware *pump* coroutine that forwards that adapter's
/// `next_message()` results into the manager fan-in (`receive_one`), while the
/// dispatcher assigns fan-in messages to bounded per-channel+conversation worker
/// queues. Each worker runs the routed agent `runner` and replies via the owning
/// adapter in message order for that one conversation; different conversations
/// can run concurrently. Empty workers exit after `options.conversation_idle_ttl`
/// and are erased before a later message for the same key is enqueued. When
/// `options.message_deadline` is set, one routed agent/reply send attempt is
/// cancelled on expiry and replaced with a short still-working reply. The
/// adapters in `manager` must already be started (`start_all`). When
/// `triggered_service` is non-null, each
/// successfully-normalized inbound message is also enqueued as an automation
/// trigger with key `channel:<channel_id>` before the direct channel reply path
/// runs; enqueue failures are reported but do not block the direct reply.
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
[[nodiscard]] async::Awaitable<core::Result<void>>
serve_channels(asio::any_io_executor executor,
               channel::ChannelManager& manager,
               channel::ChannelPromptRunner runner,
               std::vector<std::string> channel_ids,
               std::function<bool()> stop_requested = {},
               automation::AutomationService* triggered_service = nullptr,
               ServeChannelOptions options = {});

/// `orangutan --serve` entry. Loads config, starts `async::Runtime`, traps
/// SIGINT/SIGTERM on a runtime strand, co-spawns the service body, blocks the
/// calling thread until a signal arrives, then gracefully cancels the service
/// and stops the runtime. The service body always runs the IO file-view
/// watcher; when the config carries `automation.cron.jobs[]` or
/// `automation.triggered.jobs[]` it also opens
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
