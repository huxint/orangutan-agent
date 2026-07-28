// src/oran-bootstrap/serve.cpp — long-lived service mode (`--serve`).
//
// See include/oran/bootstrap/serve.hpp for the design. This TU owns the
// lifecycle (start the runtime, trap signals, co-spawn the service body, block,
// graceful cancel, stop) and races five concerns under one cancellation slot:
// the IO file-view cache watcher (always); the automation cron/triggered
// service loop plus the tool-scheduler idle-lock reaping tick (when the loaded
// config carries automation jobs or an enabled webhook listener); the webhook
// listener; and the channel ingress/dispatch loop (when the config carries
// `channels[]`). A
// disabled concern is replaced by an idle placeholder so the race always has a
// fixed operand set.

#include <oran/bootstrap/serve.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/buffer.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/cancellation_state.hpp>
#include <asio/cancellation_type.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/redirect_error.hpp>
#include <asio/signal_set.hpp>
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <oran/agent/scheduler.hpp>
#include <oran/async.hpp>
#include <oran/async/channel.hpp>
#include <oran/bootstrap/automation_cron.hpp>
#include <oran/bootstrap/automation_prompt_runner.hpp>
#include <oran/bootstrap/channel_ingress.hpp>
#include <oran/bootstrap/channel_prompt_runner.hpp>
#include <oran/bootstrap/prompt_runner.hpp>
#include <oran/bootstrap/provider_backend.hpp>
#include <oran/bootstrap/runtime_assembly.hpp>
#include <oran/bootstrap/signal_drain.hpp>
#include <oran/channel/dispatch.hpp>
#include <oran/channel/manager.hpp>
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

constexpr std::string_view kChannelDeadlineReply =
    "Still working on that. This request is taking longer than expected.";

template <typename T>
[[nodiscard]] std::size_t hash_combine(std::size_t seed, const T& value) noexcept {
  const auto h = std::hash<T>{}(value);
  return seed ^ (h + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U));
}

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

/// Idle until cancelled, then report a graceful stop. Used as the placeholder
/// operand for a disabled concern so `serve_body` can race a fixed four-operand
/// set regardless of which concerns the loaded config enables.
async::Awaitable<Result<void>> serve_idle(asio::any_io_executor executor) {
  co_await wait_until_cancelled(executor);
  co_return Result<void>{};
}

/// Pump one adapter's inbound messages into the manager fan-in until cancelled.
/// `receive_one` awaits the adapter's cancel-aware `next_message()` and forwards
/// it to the shared fan-in. A cancelled receive (this pump's own cancellation,
/// or the adapter closing) ends the loop gracefully; any other receive error is
/// reported once and ends this pump so the dispatcher keeps serving the
/// remaining adapters.
async::Awaitable<Result<void>> serve_channel_pump(channel::ChannelManager& manager,
                                                  std::string channel_id,
                                                  std::shared_ptr<std::atomic_bool> stopping) {
  for (;;) {
    if (stopping->load(std::memory_order_acquire)) {
      co_return Result<void>{};
    }
    auto pumped = co_await manager.receive_one(channel_id);
    if (!pumped) {
      if (pumped.error().kind() != core::ErrorKind::cancelled) {
        std::println(stderr, "orangutan: channel '{}' ingress ended: {}", channel_id, pumped.error());
      }
      co_return Result<void>{};
    }
  }
}

struct ChannelConversationKey {
  std::string channel_id;
  std::string conversation_id;

  friend bool operator==(const ChannelConversationKey&, const ChannelConversationKey&) = default;
};

struct ChannelConversationKeyHash {
  [[nodiscard]] std::size_t operator()(const ChannelConversationKey& key) const noexcept {
    auto seed = std::hash<std::string>{}(key.channel_id);
    return hash_combine(seed, key.conversation_id);
  }
};

[[nodiscard]] ChannelConversationKey conversation_dispatch_key(const channel::InboundMessage& message) {
  return ChannelConversationKey{.channel_id = message.channel_id, .conversation_id = message.conversation_id};
}

enum class ChannelConversationWorkerCompletion : int {
  running,
  idle,
  stopped,
};

struct ChannelWorkerSharedMetrics {
  std::atomic_uint64_t replies_sent{0};
  std::atomic_uint64_t message_timeouts{0};
  std::atomic_uint64_t dispatch_failures{0};
};

struct ChannelConversationWorkerState {
  ChannelConversationWorkerCompletion completion{ChannelConversationWorkerCompletion::running};
  bool retirement_requested{false};
};

async::Awaitable<Result<channel::DeliveryReceipt>> dispatch_channel_message(channel::ChannelManager& manager,
                                                                            const channel::ChannelPromptRunner& runner,
                                                                            channel::InboundMessage message) {
  auto request = channel::make_prompt_run_request(message);
  if (!request) {
    co_return std::unexpected(std::move(request).error());
  }

  auto result = co_await runner(std::move(*request));
  if (!result) {
    co_return std::unexpected(std::move(result).error());
  }

  co_return co_await manager.send(message.channel_id, channel::make_reply_message(message, std::move(result->text)));
}

async::Awaitable<Result<channel::DeliveryReceipt>>
dispatch_channel_message_with_deadline(channel::ChannelManager& manager,
                                       const channel::ChannelPromptRunner& runner,
                                       channel::InboundMessage message,
                                       std::optional<std::chrono::steady_clock::duration> deadline) {
  if (!deadline.has_value()) {
    co_return co_await dispatch_channel_message(manager, runner, std::move(message));
  }

  const auto channel_id = message.channel_id;
  const auto conversation_id = message.conversation_id;
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline);
  auto executor = co_await asio::this_coro::executor;

  using namespace asio::experimental::awaitable_operators;
  auto raced =
      co_await (dispatch_channel_message(manager, runner, std::move(message)) || async::sleep_for(executor, *deadline));
  if (auto* receipt = std::get_if<Result<channel::DeliveryReceipt>>(&raced); receipt != nullptr) {
    co_return std::move(*receipt);
  }

  auto timer = std::get<Result<void>>(std::move(raced));
  if (!timer) {
    co_return std::unexpected(std::move(timer).error());
  }

  co_return std::unexpected(Error::timeout(elapsed)
                                .with("source", "channel_message_deadline")
                                .with("channel_id", channel_id)
                                .with("conversation_id", conversation_id));
}

async::Awaitable<Result<channel::DeliveryReceipt>> send_channel_deadline_reply(channel::ChannelManager& manager,
                                                                               const channel::InboundMessage& message) {
  co_return co_await manager.send(message.channel_id,
                                  channel::make_reply_message(message, std::string{kChannelDeadlineReply}));
}

/// One per-channel+conversation worker. It serializes messages for a single
/// conversation while other conversation workers can await their own agent runs
/// concurrently. An idle worker requests retirement from the owning dispatcher
/// and then waits on its inbox. The dispatcher either revives it while enqueueing
/// a newly-arrived message or commits retirement by closing the inbox; the worker
/// never unilaterally exits while its map entry still accepts messages.
async::Awaitable<void>
serve_channel_conversation_worker(channel::ChannelManager& manager,
                                  asio::any_io_executor executor,
                                  channel::ChannelPromptRunner runner,
                                  std::shared_ptr<async::Channel<channel::InboundMessage>> inbox,
                                  ChannelConversationKey key,
                                  std::chrono::steady_clock::duration idle_ttl,
                                  std::shared_ptr<async::Channel<std::monostate>> progress,
                                  std::shared_ptr<ChannelConversationWorkerState> state,
                                  std::shared_ptr<ChannelWorkerSharedMetrics> metrics,
                                  std::optional<std::chrono::steady_clock::duration> message_deadline) {
  using namespace asio::experimental::awaitable_operators;
  auto reason = ChannelConversationWorkerCompletion::stopped;
  for (;;) {
    std::optional<channel::InboundMessage> message;
    auto event = co_await (inbox->receive() || async::sleep_for(executor, idle_ttl));
    if (event.index() == 1) {
      auto slept = std::get<1>(std::move(event));
      if (!slept) {
        break;  // cancelled while idle.
      }
      auto ready = inbox->try_receive();
      if (!ready) {
        break;  // closed while the idle timer won the race.
      }
      if (!ready->has_value()) {
        reason = ChannelConversationWorkerCompletion::idle;
        state->retirement_requested = true;
        [[maybe_unused]] auto signaled = progress->try_send(std::monostate{});
        auto resumed = co_await inbox->receive();
        if (!resumed) {
          break;  // the dispatcher committed retirement or shutdown began.
        }
        state->retirement_requested = false;
        reason = ChannelConversationWorkerCompletion::stopped;
        message = std::move(*resumed);
      } else {
        message = std::move(**ready);
      }
    } else {
      auto received = std::get<0>(std::move(event));
      if (!received) {
        break;
      }
      message = std::move(*received);
    }

    auto dispatch_message = *message;
    auto dispatched =
        co_await dispatch_channel_message_with_deadline(manager, runner, std::move(dispatch_message), message_deadline);
    if (!dispatched) {
      auto cancellation = co_await asio::this_coro::cancellation_state;
      if (dispatched.error().kind() == core::ErrorKind::cancelled &&
          cancellation.cancelled() != asio::cancellation_type::none) {
        break;
      }
      if (dispatched.error().kind() == core::ErrorKind::timeout) {
        metrics->message_timeouts.fetch_add(1, std::memory_order_relaxed);
        auto fallback = co_await send_channel_deadline_reply(manager, *message);
        if (fallback) {
          metrics->replies_sent.fetch_add(1, std::memory_order_relaxed);
        } else {
          metrics->dispatch_failures.fetch_add(1, std::memory_order_relaxed);
          std::println(stderr,
                       "orangutan: channel deadline reply failed for channel '{}' conversation '{}': {}",
                       key.channel_id,
                       key.conversation_id,
                       fallback.error());
        }
        [[maybe_unused]] auto signaled = progress->try_send(std::monostate{});
        continue;
      }
      metrics->dispatch_failures.fetch_add(1, std::memory_order_relaxed);
      std::println(stderr,
                   "orangutan: channel dispatch failed for channel '{}' conversation '{}': {}",
                   key.channel_id,
                   key.conversation_id,
                   dispatched.error());
    } else {
      metrics->replies_sent.fetch_add(1, std::memory_order_relaxed);
    }

    [[maybe_unused]] auto signaled = progress->try_send(std::monostate{});
  }

  state->completion = reason;
  [[maybe_unused]] auto signaled = progress->try_send(std::monostate{});
}

struct ChannelConversationWorker {
  std::shared_ptr<async::Channel<channel::InboundMessage>> inbox;
  std::shared_ptr<ChannelConversationWorkerState> state;
};

[[nodiscard]] std::string conversation_worker_name(const ChannelConversationKey& key) {
  return std::format("channel-conversation-{}:{}", key.channel_id, key.conversation_id);
}

void report_conversation_worker_outcomes(async::TaskGroupReport report) {
  if (report.outcomes_dropped > 0) {
    std::println(stderr,
                 "orangutan: channel conversation outcome retention dropped {} completed rows",
                 report.outcomes_dropped);
  }
  for (const auto& task : report.tasks) {
    if (task.status == async::TaskOutcomeStatus::failed && task.error.has_value()) {
      std::println(stderr, "orangutan: channel conversation worker '{}' failed: {}", task.name, *task.error);
    }
  }
}

/// The dispatch loop consumes the manager fan-in and assigns each message to a
/// bounded per-channel+conversation worker queue. A single worker processes one
/// conversation in order; different conversations can await their routed agent
/// runs and sends concurrently. A per-message failure is reported and that
/// worker continues, so a malformed inbound, agent-path error, or send failure
/// does not kill the daemon.
async::Awaitable<Result<void>> serve_channel_dispatch(channel::ChannelManager& manager,
                                                      asio::any_io_executor executor,
                                                      const channel::ChannelPromptRunner& runner,
                                                      std::function<bool()> stop_requested,
                                                      ServeChannelOptions options) {
  std::unordered_map<ChannelConversationKey, ChannelConversationWorker, ChannelConversationKeyHash> workers;
  std::vector<ChannelConversationWorker> retiring_workers;
  auto progress = std::make_shared<async::Channel<std::monostate>>(executor, 1);
  auto shared_metrics = std::make_shared<ChannelWorkerSharedMetrics>();
  auto metrics = ServeChannelWorkerMetrics{};

  // Conversation workers borrow `manager` and `runner` from this frame, so they
  // are owned by a bounded task group that is cancelled and joined before the
  // dispatcher returns. The group's capacity is the same hard worker cap the
  // dispatcher table enforces, which makes the bound structural rather than
  // bookkeeping-only.
  auto workers_result =
      async::TaskGroup::create(executor,
                               async::TaskGroupOptions{.max_tasks = options.max_active_conversations,
                                                       .max_completed = options.max_active_conversations});
  if (!workers_result) {
    co_return std::unexpected(std::move(workers_result).error());
  }
  auto worker_tasks = std::move(*workers_result);

  auto snapshot_metrics = [&] {
    auto snapshot = metrics;
    snapshot.active_workers = workers.size() + retiring_workers.size();
    snapshot.replies_sent = shared_metrics->replies_sent.load(std::memory_order_relaxed);
    snapshot.message_timeouts = shared_metrics->message_timeouts.load(std::memory_order_relaxed);
    snapshot.dispatch_failures = shared_metrics->dispatch_failures.load(std::memory_order_relaxed);
    return snapshot;
  };

  auto publish_metrics = [&] {
    if (!options.metrics_observer) {
      return;
    }
    try {
      const auto snapshot = snapshot_metrics();
      options.metrics_observer(snapshot);
    } catch (const std::exception& error) {
      std::println(stderr, "orangutan: channel worker metrics observer failed: {}", error.what());
    } catch (...) {
      std::println(stderr, "orangutan: channel worker metrics observer failed: unknown exception");
    }
  };

  auto erase_completed_workers = [&] {
    // Reap the dispatcher table and the group's bounded outcome retention
    // together: a worker frame that failed outright reports only through the
    // group, and draining here keeps retention near-empty for a long-lived
    // daemon instead of letting it saturate and drop rows.
    report_conversation_worker_outcomes(worker_tasks.drain_completed());
    auto removed = false;
    std::erase_if(workers, [&](const auto& entry) {
      const auto completion = entry.second.state->completion;
      if (completion == ChannelConversationWorkerCompletion::running) {
        return false;
      }
      ++metrics.workers_completed;
      if (completion == ChannelConversationWorkerCompletion::idle) {
        ++metrics.workers_evicted_idle;
      }
      removed = true;
      return true;
    });
    std::erase_if(retiring_workers, [&](const auto& worker) {
      if (worker.state->completion == ChannelConversationWorkerCompletion::running) {
        return false;
      }
      ++metrics.workers_completed;
      removed = true;
      return true;
    });
    return removed;
  };

  auto commit_requested_retirements = [&] {
    auto retired = false;
    for (auto it = workers.begin(); it != workers.end();) {
      if (!it->second.state->retirement_requested) {
        ++it;
        continue;
      }
      it->second.inbox->close();
      retiring_workers.push_back(std::move(it->second));
      it = workers.erase(it);
      ++metrics.workers_evicted_idle;
      retired = true;
    }
    return retired;
  };

  auto worker_for = [&](const channel::InboundMessage& message) -> ChannelConversationWorker* {
    if (erase_completed_workers()) {
      publish_metrics();
    }
    auto key = conversation_dispatch_key(message);
    if (auto found = workers.find(key); found != workers.end()) {
      found->second.state->retirement_requested = false;
      return &found->second;
    }

    if (workers.size() + retiring_workers.size() >= options.max_active_conversations) {
      ++metrics.enqueue_failures;
      ++metrics.conversation_overloads;
      publish_metrics();
      std::println(stderr,
                   "orangutan: channel conversation rejected at worker limit: channel='{}' conversation='{}' "
                   "max_active_conversations={}",
                   message.channel_id,
                   message.conversation_id,
                   options.max_active_conversations);
      return nullptr;
    }

    auto worker_key = key;
    auto inbox =
        std::make_shared<async::Channel<channel::InboundMessage>>(executor, options.conversation_queue_capacity);
    auto state = std::make_shared<ChannelConversationWorkerState>();
    // Spawn before publishing the worker into the table: a rejected spawn (the
    // group is at capacity or already closed) must not leave an entry whose
    // inbox nobody drains.
    auto spawned =
        worker_tasks.spawn(conversation_worker_name(worker_key),
                           [&manager,
                            executor,
                            runner,
                            inbox,
                            worker_key,
                            idle_ttl = options.conversation_idle_ttl,
                            progress,
                            state,
                            shared_metrics,
                            message_deadline = options.message_deadline]() mutable -> async::Awaitable<Result<void>> {
                             co_await serve_channel_conversation_worker(manager,
                                                                        executor,
                                                                        std::move(runner),
                                                                        std::move(inbox),
                                                                        std::move(worker_key),
                                                                        idle_ttl,
                                                                        std::move(progress),
                                                                        std::move(state),
                                                                        std::move(shared_metrics),
                                                                        message_deadline);
                             co_return Result<void>{};
                           });
    if (!spawned) {
      ++metrics.enqueue_failures;
      ++metrics.conversation_overloads;
      publish_metrics();
      std::println(stderr,
                   "orangutan: channel conversation worker spawn rejected: channel='{}' conversation='{}': {}",
                   message.channel_id,
                   message.conversation_id,
                   spawned.error());
      return nullptr;
    }

    auto [inserted, _] =
        workers.emplace(std::move(key),
                        ChannelConversationWorker{.inbox = std::move(inbox), .state = std::move(state)});
    ++metrics.workers_created;
    metrics.max_active_workers = std::max(metrics.max_active_workers, workers.size());
    publish_metrics();
    return &inserted->second;
  };

  using namespace asio::experimental::awaitable_operators;
  for (;;) {
    if (stop_requested && stop_requested()) {
      break;  // cooperative early-out before the next receive.
    }
    auto event = co_await (manager.inbound().receive() || progress->receive());
    if (event.index() == 1) {
      auto seen_progress = std::get<1>(std::move(event));
      if (!seen_progress && seen_progress.error().kind() == core::ErrorKind::cancelled) {
        break;
      }
      // A committed idle retirement has already closed the worker inbox, so
      // completion requires no external work. Drain it before accepting the
      // next inbound message: the hard worker cap then counts every spawned
      // worker without rejecting a message merely because its predecessor is
      // between close() and its final completion wake.
      //
      // Re-commit on every drain iteration. `progress` is a single-slot wake,
      // so a retirement another worker requests *during* this drain has its
      // wake consumed here and would never reach the loop head again; the
      // request flag itself is what this branch acts on, and it is set before
      // the wake is sent, so re-checking it here cannot miss one.
      bool retired = false;
      bool erased = false;
      for (;;) {
        retired = commit_requested_retirements() || retired;
        erased = erase_completed_workers() || erased;
        const bool draining = std::ranges::any_of(retiring_workers, [](const auto& worker) {
          return worker.state->completion == ChannelConversationWorkerCompletion::running;
        });
        if (!draining) {
          break;
        }
        auto completed = co_await progress->receive();
        if (!completed) {
          break;
        }
      }
      if (retired || erased) {
        publish_metrics();
      }
      continue;  // A worker completed one message; re-check stop_requested.
    }

    auto message = std::get<0>(std::move(event));
    if (!message && message.error().kind() == core::ErrorKind::cancelled) {
      break;  // parent stop or closed fan-in — graceful.
    }
    if (!message) {
      std::println(stderr, "orangutan: channel dispatch receive failed: {}", message.error());
      continue;
    }

    auto* worker = worker_for(*message);
    if (worker == nullptr) {
      continue;
    }
    auto sent = co_await worker->inbox->send(std::move(*message));
    if (!sent && sent.error().kind() == core::ErrorKind::cancelled) {
      break;
    }
    if (!sent) {
      ++metrics.enqueue_failures;
      publish_metrics();
      std::println(stderr, "orangutan: channel conversation enqueue failed: {}", sent.error());
    } else {
      ++metrics.messages_enqueued;
      publish_metrics();
    }
  }

  // Close every inbox first so a worker parked on a receive wakes with a closed
  // channel rather than a cancellation, then cancel and join the group. The
  // join is the ownership boundary: workers borrow `manager` and `runner` from
  // this frame, so none may still be running when it returns.
  co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
  for (auto& [_, worker] : workers) {
    worker.inbox->close();
  }
  for (auto& worker : retiring_workers) {
    worker.inbox->close();
  }
  worker_tasks.request_stop();
  auto worker_report = co_await worker_tasks.join();
  if (!worker_report) {
    std::println(stderr, "orangutan: channel conversation worker join failed: {}", worker_report.error());
  } else {
    report_conversation_worker_outcomes(std::move(*worker_report));
  }
  if (erase_completed_workers()) {
    publish_metrics();
  }

  co_return Result<void>{};
}

[[nodiscard]] std::string channel_trigger_key(std::string_view channel_id) {
  auto key = std::string{"channel:"};
  key += channel_id;
  return key;
}

[[nodiscard]] channel::ChannelPromptRunner with_channel_trigger_enqueue(channel::ChannelPromptRunner runner,
                                                                        automation::AutomationService& service) {
  return channel::ChannelPromptRunner{
      [runner = std::move(runner), service = &service](channel::ChannelPromptRunRequest request) mutable
          -> async::Awaitable<core::Result<channel::ChannelPromptRunResult>> {
        auto trigger_key = channel_trigger_key(request.channel_id);
        auto enqueued = co_await service->enqueue_triggered(automation::TriggeredQueueEnqueueRequest{
            .trigger_key = std::move(trigger_key),
            .received_at = request.received_at,
            .job_limit = 100,
        });
        if (!enqueued) {
          std::println(stderr,
                       "orangutan: channel automation trigger enqueue failed for '{}': {}",
                       request.channel_id,
                       enqueued.error());
        } else if (enqueued->dropped_count > 0) {
          std::println(stderr,
                       "orangutan: channel automation trigger '{}' dropped {} job(s)",
                       enqueued->intake.trigger_key,
                       enqueued->dropped_count);
        }

        co_return co_await runner(std::move(request));
      }};
}

[[nodiscard]] std::string trim_http_line_suffix(std::string line) {
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  return line;
}

[[nodiscard]] std::string ascii_lower(std::string text) {
  std::ranges::transform(text, text.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return text;
}

[[nodiscard]] std::string trim_http_ows(std::string_view text) {
  const auto first = text.find_first_not_of(" \t");
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(" \t");
  return std::string{text.substr(first, last - first + 1)};
}

struct ParsedWebhookHttpRequest {
  std::string method;
  std::string target;
  std::string body;
};

class WebhookSocketDeadline {
public:
  WebhookSocketDeadline(const std::shared_ptr<asio::ip::tcp::socket>& socket, std::chrono::milliseconds timeout)
      : timer_{socket->get_executor()}, expired_{std::make_shared<std::atomic_bool>(false)} {
    timer_.expires_after(timeout);
    timer_.async_wait([socket = std::weak_ptr{socket}, expired = expired_](const asio::error_code& ec) {
      if (ec) {
        return;
      }
      expired->store(true, std::memory_order_release);
      if (auto locked = socket.lock()) {
        asio::error_code ignored;
        locked->cancel(ignored);
      }
    });
  }

  [[nodiscard]] bool finish() {
    static_cast<void>(timer_.cancel());
    return expired_->load(std::memory_order_acquire);
  }

private:
  asio::steady_timer timer_;
  std::shared_ptr<std::atomic_bool> expired_;
};

[[nodiscard]] Result<std::size_t> parse_content_length(std::string_view value) {
  auto length = std::uint64_t{};
  auto parsed = std::from_chars(value.data(), value.data() + value.size(), length);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
    return std::unexpected(Error::invalid_argument("invalid Content-Length header"));
  }
  if (length > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return std::unexpected(Error::invalid_argument("Content-Length is too large"));
  }
  return static_cast<std::size_t>(length);
}

[[nodiscard]] async::Awaitable<Result<ParsedWebhookHttpRequest>>
read_webhook_http_request(const std::shared_ptr<asio::ip::tcp::socket>& socket,
                          std::size_t max_payload_bytes,
                          std::size_t max_header_bytes,
                          std::chrono::milliseconds header_timeout,
                          std::chrono::milliseconds read_timeout) {
  auto received = std::string{};
  auto read_headers = [&]() -> async::Awaitable<Result<std::size_t>> {
    asio::error_code ec;
    auto bytes = co_await asio::async_read_until(*socket,
                                                 asio::dynamic_buffer(received, max_header_bytes),
                                                 "\r\n\r\n",
                                                 asio::redirect_error(asio::use_awaitable, ec));
    if (!ec) {
      co_return bytes;
    }
    if (ec == asio::error::not_found) {
      co_return std::unexpected(Error::invalid_argument("webhook headers exceed max_header_bytes")
                                    .with("max_header_bytes", std::to_string(max_header_bytes)));
    }
    co_return std::unexpected(Error::io("failed to read webhook request headers").with("asio_error", ec.message()));
  };

  auto header_deadline = WebhookSocketDeadline{socket, header_timeout};
  auto header_bytes = co_await read_headers();
  if (header_deadline.finish()) {
    co_return std::unexpected(Error::timeout(header_timeout).with("phase", "webhook_headers"));
  }
  if (!header_bytes) {
    co_return std::unexpected(std::move(header_bytes).error());
  }

  auto input = std::istringstream{received.substr(0, *header_bytes)};
  auto request_line = std::string{};
  if (!std::getline(input, request_line)) {
    co_return std::unexpected(Error::invalid_argument("missing HTTP request line"));
  }
  request_line = trim_http_line_suffix(std::move(request_line));

  auto parsed = std::istringstream{request_line};
  auto request = ParsedWebhookHttpRequest{};
  auto version = std::string{};
  parsed >> request.method >> request.target >> version;
  if (request.method.empty() || request.target.empty() || !version.starts_with("HTTP/")) {
    co_return std::unexpected(Error::invalid_argument("malformed HTTP request line"));
  }

  auto content_length = std::size_t{0};
  for (auto line = std::string{}; std::getline(input, line);) {
    line = trim_http_line_suffix(std::move(line));
    if (line.empty()) {
      break;
    }
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    const auto name = ascii_lower(line.substr(0, colon));
    if (name != "content-length") {
      continue;
    }
    auto length = parse_content_length(trim_http_ows(std::string_view{line}.substr(colon + 1)));
    if (!length) {
      co_return std::unexpected(std::move(length.error()));
    }
    content_length = *length;
  }

  if (content_length > max_payload_bytes) {
    co_return std::unexpected(Error::invalid_argument("webhook payload exceeds max_payload_bytes")
                                  .with("content_length", std::to_string(content_length))
                                  .with("max_payload_bytes", std::to_string(max_payload_bytes)));
  }

  auto request_body_bytes = received.size() - *header_bytes;
  if (request_body_bytes > content_length) {
    request_body_bytes = content_length;
  }
  request.body.assign(received.data() + *header_bytes, request_body_bytes);

  if (request.body.size() < content_length) {
    const auto body_bytes_received = request.body.size();
    request.body.resize(content_length);
    auto read_body = [&]() -> async::Awaitable<Result<std::size_t>> {
      asio::error_code ec;
      auto bytes = co_await asio::async_read(
          *socket,
          asio::buffer(request.body.data() + body_bytes_received, content_length - body_bytes_received),
          asio::redirect_error(asio::use_awaitable, ec));
      if (ec) {
        co_return std::unexpected(Error::io("failed to read webhook request body").with("asio_error", ec.message()));
      }
      co_return bytes;
    };
    auto body_deadline = WebhookSocketDeadline{socket, read_timeout};
    auto body_bytes = co_await read_body();
    if (body_deadline.finish()) {
      co_return std::unexpected(Error::timeout(read_timeout).with("phase", "webhook_body"));
    }
    if (!body_bytes) {
      co_return std::unexpected(std::move(body_bytes).error());
    }
  }

  co_return request;
}

[[nodiscard]] std::optional<std::string> webhook_key_from_target(std::string target, std::string_view path_prefix) {
  if (const auto query = target.find('?'); query != std::string::npos) {
    target.erase(query);
  }
  if (!target.starts_with(path_prefix)) {
    return std::nullopt;
  }
  auto webhook_key = target.substr(path_prefix.size());
  if (webhook_key.empty() || webhook_key.contains('/')) {
    return std::nullopt;
  }
  return webhook_key;
}

[[nodiscard]] std::string json_string_literal(std::string_view text) {
  auto escaped = std::string{};
  escaped.reserve(text.size() + 2);
  escaped.push_back('"');
  for (const auto ch : text) {
    switch (ch) {
      case '"':
        escaped += "\\\"";
        break;
      case '\\':
        escaped += "\\\\";
        break;
      case '\b':
        escaped += "\\b";
        break;
      case '\f':
        escaped += "\\f";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20U) {
          escaped += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(ch)));
        } else {
          escaped.push_back(ch);
        }
        break;
    }
  }
  escaped.push_back('"');
  return escaped;
}

[[nodiscard]] std::string webhook_http_response(unsigned status, std::string_view reason, std::string body) {
  return std::format("HTTP/1.1 {} {}\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: {}\r\n"
                     "Connection: close\r\n"
                     "\r\n"
                     "{}",
                     status,
                     reason,
                     body.size(),
                     body);
}

async::Awaitable<void> write_webhook_http_response(const std::shared_ptr<asio::ip::tcp::socket>& socket,
                                                   unsigned status,
                                                   std::string_view reason,
                                                   std::string body,
                                                   std::chrono::milliseconds timeout) {
  auto response = webhook_http_response(status, reason, std::move(body));
  auto deadline = WebhookSocketDeadline{socket, timeout};
  asio::error_code ec;
  [[maybe_unused]] auto written =
      co_await asio::async_write(*socket, asio::buffer(response), asio::redirect_error(asio::use_awaitable, ec));
  static_cast<void>(deadline.finish());
  socket->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
  socket->close(ec);
}

async::Awaitable<void> handle_webhook_connection(asio::ip::tcp::socket socket,
                                                 automation::AutomationService& service,
                                                 ServeWebhookOptions options) {
  auto connection = std::make_shared<asio::ip::tcp::socket>(std::move(socket));
  auto request = co_await read_webhook_http_request(connection,
                                                    options.max_payload_bytes,
                                                    options.max_header_bytes,
                                                    options.header_timeout,
                                                    options.read_timeout);
  if (!request) {
    const auto status = request.error().kind() == core::ErrorKind::timeout        ? 408U
                        : request.error().message().contains("max_payload_bytes") ? 413U
                        : request.error().message().contains("max_header_bytes")  ? 431U
                                                                                  : 400U;
    co_await write_webhook_http_response(connection,
                                         status,
                                         status == 408U   ? "Request Timeout"
                                         : status == 413U ? "Payload Too Large"
                                         : status == 431U ? "Request Header Fields Too Large"
                                                          : "Bad Request",
                                         R"({"ok":false})",
                                         options.write_timeout);
    co_return;
  }

  if (request->method != "POST") {
    co_await write_webhook_http_response(connection,
                                         405,
                                         "Method Not Allowed",
                                         R"({"ok":false})",
                                         options.write_timeout);
    co_return;
  }

  auto webhook_key = webhook_key_from_target(request->target, options.path_prefix);
  if (!webhook_key) {
    co_await write_webhook_http_response(connection, 404, "Not Found", R"({"ok":false})", options.write_timeout);
    co_return;
  }

  auto producer = automation::WebhookProducer{service};
  auto triggered = co_await producer.trigger(automation::WebhookTriggerRequest{
      .webhook_key = std::move(*webhook_key),
      .payload = request->body.empty() ? std::nullopt : std::optional<std::string>{std::move(request->body)},
      .received_at = core::time::now_utc(),
      .job_limit = options.job_limit,
  });
  if (!triggered) {
    const auto status = triggered.error().kind() == core::ErrorKind::invalid_argument ? 400U : 500U;
    co_await write_webhook_http_response(connection,
                                         status,
                                         status == 400U ? "Bad Request" : "Internal Server Error",
                                         R"({"ok":false})",
                                         options.write_timeout);
    co_return;
  }

  auto body = std::format(R"({{"ok":true,"trigger_key":{},"matched":{},"enqueued":{},"dropped":{}}})",
                          json_string_literal(triggered->trigger_key),
                          triggered->enqueue.intake.matched_count,
                          triggered->enqueue.enqueued_count,
                          triggered->enqueue.dropped_count);
  co_await write_webhook_http_response(connection, 202, "Accepted", std::move(body), options.write_timeout);
}

[[nodiscard]] async::Awaitable<Result<asio::ip::tcp::socket>>
accept_webhook_connection(asio::ip::tcp::acceptor& acceptor) {
  asio::error_code ec;
  auto socket = co_await acceptor.async_accept(asio::redirect_error(asio::use_awaitable, ec));
  if (!ec) {
    co_return socket;
  }
  if (ec == asio::error::operation_aborted || ec == asio::error::bad_descriptor) {
    co_return std::unexpected(Error::cancelled().with("source", "webhook_accept").with("asio_error", ec.message()));
  }
  co_return std::unexpected(Error::io("failed to accept webhook connection").with("asio_error", ec.message()));
}

void report_webhook_worker_outcomes(async::TaskGroupReport report) {
  if (report.outcomes_dropped > 0) {
    std::println(stderr,
                 "orangutan: webhook connection outcome retention dropped {} completed rows",
                 report.outcomes_dropped);
  }
  for (const auto& task : report.tasks) {
    if (task.status == async::TaskOutcomeStatus::failed && task.error.has_value()) {
      std::println(stderr, "orangutan: webhook connection '{}' failed: {}", task.name, *task.error);
    }
  }
}

async::Awaitable<Result<void>> stop_webhook_listener(std::shared_ptr<asio::ip::tcp::acceptor> acceptor,
                                                     std::shared_ptr<std::atomic_bool> stop_watcher_done,
                                                     async::TaskGroup& workers) {
  stop_watcher_done->store(true, std::memory_order_release);
  asio::error_code ignored;
  acceptor->close(ignored);
  workers.request_stop();
  auto report = co_await workers.join();
  if (!report) {
    co_return std::unexpected(std::move(report).error());
  }
  report_webhook_worker_outcomes(std::move(*report));
  co_return Result<void>{};
}

async::Awaitable<void> poke_webhook_acceptor_on_stop(asio::any_io_executor executor,
                                                     asio::ip::tcp::endpoint endpoint,
                                                     std::function<bool()> stop_requested,
                                                     std::shared_ptr<std::atomic_bool> done) {
  while (!done->load(std::memory_order_acquire)) {
    if (stop_requested && stop_requested()) {
      auto socket = asio::ip::tcp::socket{executor};
      asio::error_code ignored;
      socket.connect(endpoint, ignored);
      co_return;
    }
    auto slept = co_await async::sleep_for(executor, std::chrono::milliseconds{50});
    if (!slept) {
      co_return;
    }
  }
}

/// The composed service body. A free coroutine (not a capture-by-reference
/// lambda) so its inputs are moved into the coroutine frame and cannot dangle
/// after the spawning full-expression. Opens automation persistence and starts
/// channel adapters as configured, then races the watcher, the automation loop,
/// the scheduler reaping tick, the webhook listener, and the channel
/// ingress/dispatch loop — each replaced by an idle placeholder when its
/// concern is disabled, all under the caller's cancellation slot. Automation
/// persistence that cannot open (or seeds that cannot apply) and channel
/// adapters that cannot start are non-fatal: report once and degrade that
/// concern to idle so a signal still stops the rest cleanly.
async::Awaitable<Result<void>> serve_body(asio::any_io_executor executor,
                                          ServeOptions watch_options,
                                          bool automation_enabled,
                                          std::string automation_db,
                                          std::vector<automation::UpsertCronJobRequest> cron_seeds,
                                          std::vector<automation::UpsertTriggeredJobRequest> triggered_seeds,
                                          automation::CronJobHandler cron_handler,
                                          automation::TriggeredJobHandler triggered_handler,
                                          ServeAutomationOptions automation_options,
                                          agent::ToolScheduler* scheduler,
                                          ServeSchedulerReapOptions reap_options,
                                          bool webhooks_enabled,
                                          ServeWebhookOptions webhook_options,
                                          bool channels_enabled,
                                          channel::ChannelManager* channel_manager,
                                          channel::ChannelPromptRunner channel_runner,
                                          std::vector<std::string> channel_ids,
                                          ServeChannelOptions channel_options,
                                          std::function<bool()> stop_requested) {
  // Open automation persistence (non-fatal). On any failure the automation and
  // reaping concerns degrade to idle, but the watcher and channels keep serving.
  std::optional<automation::AutomationRuntime> automation_runtime;
  std::optional<automation::AutomationService> automation_service;
  bool automation_active = false;
  if (automation_enabled) {
    auto opened = co_await automation::AutomationRuntime::open(
        executor,
        automation::AutomationRuntimeOptions{.database_path = std::move(automation_db)});
    if (!opened) {
      std::println(stderr, "orangutan: automation runtime unavailable, serving without it: {}", opened.error());
    } else {
      automation_runtime.emplace(std::move(*opened));
      if (auto seeded = co_await automation_runtime->apply_cron_job_seeds(std::move(cron_seeds)); !seeded) {
        std::println(stderr, "orangutan: automation cron seeds failed, serving without automation: {}", seeded.error());
      } else if (auto triggered_seeded =
                     co_await automation_runtime->apply_triggered_job_seeds(std::move(triggered_seeds));
                 !triggered_seeded) {
        std::println(stderr,
                     "orangutan: automation triggered seeds failed, serving without automation: {}",
                     triggered_seeded.error());
      } else {
        automation_service.emplace(automation_runtime->automation_service());
        automation_active = true;
      }
    }
  }

  // Start channel adapters (non-fatal). On failure the channel concern degrades
  // to idle while the rest of the service keeps running.
  bool channels_active = false;
  if (channels_enabled && channel_manager != nullptr) {
    if (auto started = co_await channel_manager->start_all(); !started) {
      if (auto stopped = co_await channel_manager->stop_all(); !stopped) {
        std::println(stderr, "orangutan: channel adapters failed to stop after start failure: {}", stopped.error());
      }
      std::println(stderr,
                   "orangutan: channel adapters failed to start, serving without channels: {}",
                   started.error());
    } else {
      channels_active = true;
    }
  }

  using namespace asio::experimental::awaitable_operators;
  // A fixed five-operand race under one cancellation slot: the watcher plus each
  // optional concern (or an idle placeholder when disabled), so a single signal
  // — or any concern's self-stop — ends them together. `stop_requested` is
  // copied into each predicate consumer because the evaluation order of `||`
  // operands is unspecified, so moving into one could hand another a moved-from
  // (empty) function. Each conditional operand only evaluates the taken branch,
  // so the real concern's borrowed state is touched only when it is active.
  co_await (serve_run(executor, std::move(watch_options)) ||
            (automation_active ? serve_automation(executor,
                                                  *automation_service,
                                                  std::move(cron_handler),
                                                  std::move(triggered_handler),
                                                  automation_options,
                                                  stop_requested)
                               : serve_idle(executor)) ||
            (automation_active && scheduler != nullptr
                 ? serve_scheduler_reaping(executor, *scheduler, reap_options, stop_requested)
                 : serve_idle(executor)) ||
            (automation_active && webhooks_enabled
                 ? serve_webhooks(executor, *automation_service, std::move(webhook_options), stop_requested)
                 : serve_idle(executor)) ||
            (channels_active ? serve_channels(executor,
                                              *channel_manager,
                                              std::move(channel_runner),
                                              std::move(channel_ids),
                                              stop_requested,
                                              automation_active ? &*automation_service : nullptr,
                                              std::move(channel_options))
                             : serve_idle(executor)));

  co_return Result<void>{};
}

}  // namespace

std::string format_serve_channel_worker_metrics(const ServeChannelWorkerMetrics& snapshot) {
  return std::format("orangutan: channel worker metrics active={} max={} created={} completed={} idle_evicted={} "
                     "enqueued={} replies={} timeouts={} dispatch_failures={} enqueue_failures={} overloads={}",
                     snapshot.active_workers,
                     snapshot.max_active_workers,
                     snapshot.workers_created,
                     snapshot.workers_completed,
                     snapshot.workers_evicted_idle,
                     snapshot.messages_enqueued,
                     snapshot.replies_sent,
                     snapshot.message_timeouts,
                     snapshot.dispatch_failures,
                     snapshot.enqueue_failures,
                     snapshot.conversation_overloads);
}

ServeChannelMetricsLogSink::ServeChannelMetricsLogSink(ServeChannelMetricsLogSinkOptions options)
    : options_(std::move(options)) {}

void ServeChannelMetricsLogSink::operator()(const ServeChannelWorkerMetrics& snapshot) {
  if (last_snapshot_.has_value() && *last_snapshot_ == snapshot) {
    return;
  }
  last_snapshot_ = snapshot;
  auto line = format_serve_channel_worker_metrics(snapshot);
  if (options_.emit_line) {
    options_.emit_line(std::move(line));
    return;
  }
  std::println(stderr, "{}", line);
}

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

async::Awaitable<Result<void>> serve_webhooks(asio::any_io_executor executor,
                                              automation::AutomationService& service,
                                              ServeWebhookOptions options,
                                              std::function<bool()> stop_requested) {
  if (options.bind_host.empty()) {
    co_return std::unexpected(Error::invalid_argument("webhook listener bind_host must be non-empty"));
  }
  if (options.path_prefix.empty() || !options.path_prefix.starts_with('/') || !options.path_prefix.ends_with('/')) {
    co_return std::unexpected(Error::invalid_argument("webhook listener path_prefix must start and end with /"));
  }
  if (options.job_limit == 0) {
    co_return std::unexpected(Error::invalid_argument("webhook listener job_limit must be positive"));
  }
  if (options.max_payload_bytes == 0) {
    co_return std::unexpected(Error::invalid_argument("webhook listener max_payload_bytes must be positive"));
  }
  if (options.max_header_bytes == 0) {
    co_return std::unexpected(Error::invalid_argument("webhook listener max_header_bytes must be positive"));
  }
  if (options.max_connections == 0) {
    co_return std::unexpected(Error::invalid_argument("webhook listener max_connections must be positive"));
  }
  if (options.header_timeout <= std::chrono::milliseconds::zero() ||
      options.read_timeout <= std::chrono::milliseconds::zero() ||
      options.write_timeout <= std::chrono::milliseconds::zero()) {
    co_return std::unexpected(Error::invalid_argument("webhook listener deadlines must be positive"));
  }

  asio::error_code ec;
  const auto address = asio::ip::make_address(options.bind_host, ec);
  if (ec) {
    co_return std::unexpected(
        Error::config("webhook listener bind_host must be a numeric IP address").with("bind_host", options.bind_host));
  }
  if (!address.is_loopback()) {
    co_return std::unexpected(
        Error::config("webhook listener bind_host must be loopback until authentication is configured")
            .with("bind_host", options.bind_host));
  }

  auto acceptor = std::make_shared<asio::ip::tcp::acceptor>(executor);
  const auto endpoint = asio::ip::tcp::endpoint{address, options.port};
  acceptor->open(endpoint.protocol(), ec);
  if (ec) {
    co_return std::unexpected(Error::io("failed to open webhook listener").with("asio_error", ec.message()));
  }
  acceptor->set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
  if (ec) {
    co_return std::unexpected(Error::io("failed to configure webhook listener").with("asio_error", ec.message()));
  }
  acceptor->bind(endpoint, ec);
  if (ec) {
    co_return std::unexpected(Error::io("failed to bind webhook listener").with("asio_error", ec.message()));
  }
  acceptor->listen(asio::socket_base::max_listen_connections, ec);
  if (ec) {
    co_return std::unexpected(Error::io("failed to listen for webhooks").with("asio_error", ec.message()));
  }
  if (options.bound_observer) {
    try {
      options.bound_observer(acceptor->local_endpoint().port());
    } catch (const std::exception& error) {
      std::println(stderr, "orangutan: webhook bound observer failed: {}", error.what());
    } catch (...) {
      std::println(stderr, "orangutan: webhook bound observer failed: unknown exception");
    }
  }

  const auto local_endpoint = acceptor->local_endpoint();
  auto stop_watcher_done = std::make_shared<std::atomic_bool>(false);
  asio::co_spawn(executor,
                 poke_webhook_acceptor_on_stop(executor, local_endpoint, stop_requested, stop_watcher_done),
                 asio::detached);

  auto workers_result = async::TaskGroup::create(
      executor,
      async::TaskGroupOptions{.max_tasks = options.max_connections, .max_completed = options.max_connections});
  if (!workers_result) {
    co_return std::unexpected(std::move(workers_result).error());
  }
  auto workers = std::move(*workers_result);
  auto next_connection_id = std::uint64_t{};

  for (;;) {
    report_webhook_worker_outcomes(workers.drain_completed());
    if (stop_requested && stop_requested()) {
      co_return co_await stop_webhook_listener(acceptor, stop_watcher_done, workers);
    }

    auto accepted = co_await accept_webhook_connection(*acceptor);
    if (!accepted) {
      if (accepted.error().kind() == core::ErrorKind::cancelled) {
        co_return co_await stop_webhook_listener(acceptor, stop_watcher_done, workers);
      }
      std::println(stderr, "orangutan: webhook accept failed: {}", accepted.error());
      continue;
    }

    if (stop_requested && stop_requested()) {
      asio::error_code ignored;
      accepted->close(ignored);
      co_return co_await stop_webhook_listener(acceptor, stop_watcher_done, workers);
    }

    if (workers.active_tasks() >= options.max_connections) {
      auto overloaded = std::make_shared<asio::ip::tcp::socket>(std::move(*accepted));
      co_await write_webhook_http_response(overloaded,
                                           503,
                                           "Service Unavailable",
                                           R"({"ok":false})",
                                           options.write_timeout);
      continue;
    }

    auto task_name = std::format("connection-{}", next_connection_id++);
    auto spawned = workers.spawn(
        std::move(task_name),
        [socket = std::move(*accepted), service = &service, options]() mutable -> async::Awaitable<Result<void>> {
          co_await handle_webhook_connection(std::move(socket), *service, std::move(options));
          co_return Result<void>{};
        });
    if (!spawned) {
      std::println(stderr, "orangutan: webhook connection worker spawn failed: {}", spawned.error());
    }
  }
}

async::Awaitable<Result<void>> serve_channels(asio::any_io_executor executor,
                                              channel::ChannelManager& manager,
                                              channel::ChannelPromptRunner runner,
                                              std::vector<std::string> channel_ids,
                                              std::function<bool()> stop_requested,
                                              automation::AutomationService* triggered_service,
                                              ServeChannelOptions options) {
  // Reject a null runner up front: `dispatch_one` returns `invalid_argument`
  // for a null runner *without* consuming a fan-in message, which would hot-spin
  // the dispatch loop. run_serve and the tests always supply a real runner.
  if (!runner) {
    co_return std::unexpected(Error::invalid_argument("channel prompt runner is null"));
  }
  if (options.conversation_queue_capacity == 0) {
    co_return std::unexpected(Error::invalid_argument("channel conversation queue capacity must be positive"));
  }
  if (options.max_active_conversations == 0) {
    co_return std::unexpected(Error::invalid_argument("channel max active conversations must be positive"));
  }
  if (options.conversation_idle_ttl < std::chrono::steady_clock::duration::zero()) {
    co_return std::unexpected(Error::invalid_argument("channel conversation idle ttl must be non-negative"));
  }
  if (options.message_deadline.has_value() &&
      *options.message_deadline <= std::chrono::steady_clock::duration::zero()) {
    co_return std::unexpected(Error::invalid_argument("channel message deadline must be positive"));
  }
  auto dispatch_runner = std::move(runner);
  if (triggered_service != nullptr) {
    dispatch_runner = with_channel_trigger_enqueue(std::move(dispatch_runner), *triggered_service);
  }

  // Own the background fan-in loop (design-docs/channel-abstraction.md): one
  // pump coroutine per adapter forwards `next_message()` into the shared fan-in
  // while the dispatcher consumes it. Pumps live in a bounded TaskGroup so
  // shutdown can cancel and join every child before this frame returns (each
  // pump borrows `manager`).
  const std::size_t pump_count = channel_ids.size();
  auto stopping = std::make_shared<std::atomic_bool>(false);
  auto pumps_result =
      async::TaskGroup::create(executor,
                               async::TaskGroupOptions{.max_tasks = std::max(pump_count, std::size_t{1}),
                                                       .max_completed = std::max(pump_count, std::size_t{1})});
  if (!pumps_result) {
    co_return std::unexpected(std::move(pumps_result).error());
  }
  auto pumps = std::move(*pumps_result);
  for (auto& channel_id : channel_ids) {
    auto task_name = "channel-pump-" + channel_id;
    auto spawned = pumps.spawn(
        std::move(task_name),
        [&manager, channel_id = std::move(channel_id), stopping]() mutable -> async::Awaitable<Result<void>> {
          co_return co_await serve_channel_pump(manager, std::move(channel_id), std::move(stopping));
        });
    if (!spawned) {
      stopping->store(true, std::memory_order_release);
      pumps.request_stop();
      [[maybe_unused]] auto joined = co_await pumps.join();
      co_return std::unexpected(std::move(spawned).error());
    }
  }

  // The dispatcher is the awaited body. When the parent cancels this coroutine
  // (the serve_body `||` race), this await's fan-in receive returns `cancelled`
  // and control returns here for cleanup.
  auto dispatcher = asio::make_strand(executor);
  auto dispatched = co_await asio::co_spawn(
      dispatcher,
      serve_channel_dispatch(manager, dispatcher, dispatch_runner, std::move(stop_requested), options),
      asio::use_awaitable);

  // Stop adapters and join the pumps with cancellation disabled, so an
  // already-fired parent cancellation cannot abandon the TaskGroup join
  // mid-drain. The explicit stopping flag covers the window where a pump has
  // just finished one `next_message()` and has not yet installed the
  // cancellation handler for the next one, while `stop_all()` wakes
  // adapter-owned receives (QQ closes its gateway transport; MockChannel
  // closes its bounded inbound queue). `request_stop()` then cancels any pump
  // still parked on a receive.
  co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
  stopping->store(true, std::memory_order_release);
  auto stopped = co_await manager.stop_all();
  if (!stopped) {
    std::println(stderr, "orangutan: channel adapters failed to stop cleanly: {}", stopped.error());
  }
  pumps.request_stop();
  auto pump_report = co_await pumps.join();
  if (!pump_report) {
    std::println(stderr, "orangutan: channel pump join failed: {}", pump_report.error());
  } else {
    for (const auto& task : pump_report->tasks) {
      if (task.error.has_value() && task.error->kind() != core::ErrorKind::cancelled) {
        std::println(stderr, "orangutan: channel pump '{}' ended: {}", task.name, *task.error);
      }
    }
  }

  if (!dispatched) {
    co_return dispatched;
  }
  co_return stopped;
}

Result<int> run_serve(const BootstrapOptions& options) {
  auto loaded = load_config(options);
  if (!loaded) {
    return std::unexpected(std::move(loaded).error());
  }
  const auto& cfg = loaded->value;

  // Config-authored cron/triggered seeds gate the automation concern: with none,
  // `--serve` avoids the provider and `automation.db` unless channels need an
  // agent.
  auto cron_seeds = cron_jobs_from(cfg);
  if (!cron_seeds) {
    return std::unexpected(std::move(cron_seeds).error());
  }
  auto triggered_seeds = triggered_jobs_from(cfg);
  if (!triggered_seeds) {
    return std::unexpected(std::move(triggered_seeds).error());
  }
  const bool webhooks_enabled = cfg.automation().webhooks.listener.enabled;
  const bool automation_enabled = !cron_seeds->empty() || !triggered_seeds->empty() || webhooks_enabled;
  const auto cron_job_count = cron_seeds->size();
  const auto triggered_job_count = triggered_seeds->size();

  // Config-authored `channels[]` gate the channel ingress/dispatch concern, the
  // same way cron jobs gate automation: with none, `--serve` builds no channel
  // manager and that concern stays idle (CI-identical).
  const bool channels_enabled = !cfg.channels().empty();

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

  // Runtime wiring lives on this stack so the runtime assembly, provider
  // backend, the shared tool registry + scheduler, and the channel manager (all
  // borrowed by the service coroutine) outlive it — the coroutine completes
  // before `service_future.get()` returns, i.e. before these are destroyed.
  std::optional<RuntimeAssembly> assembly;
  std::optional<HttpProviderBackend> live_backend;
  std::optional<provider::FakeProvider> offline_provider;
  std::optional<tool::Registry> shared_registry;
  std::optional<agent::ToolScheduler> shared_scheduler;
  automation::CronJobHandler cron_handler;
  automation::TriggeredJobHandler triggered_handler;
  std::string automation_db;
  bool provider_live = false;
  auto webhook_options = ServeWebhookOptions{};
  if (webhooks_enabled) {
    const auto& listener = cfg.automation().webhooks.listener;
    webhook_options.bind_host = listener.bind_host;
    webhook_options.port = listener.port;
    webhook_options.path_prefix = listener.path_prefix;
    webhook_options.max_payload_bytes = static_cast<std::size_t>(listener.max_payload_bytes);
    webhook_options.job_limit = static_cast<std::size_t>(listener.job_limit);
  }

  std::optional<channel::ChannelManager> channel_manager;
  channel::ChannelPromptRunner channel_runner;
  std::vector<std::string> channel_ids;

  // Automation and channels both run agents, so both need the runtime assembly
  // and a provider. Build them once and share when both are enabled.
  const bool needs_runtime = automation_enabled || channels_enabled;
  provider::System* system = nullptr;
  provider::Route route{};
  if (needs_runtime) {
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

    // A configured `default` route drives a live provider; otherwise an offline
    // scripted fake keeps the loops usable without credentials (the same offline
    // posture as `--desktop`), so CI stays secret-free.
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
      provider_live = true;
    } else {
      offline_provider.emplace(serve_offline_plan());
      system = &*offline_provider;
      route = serve_offline_route();
    }
  }

  if (automation_enabled) {
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

    auto prompt_runner = make_automation_agent_prompt_runner(AutomationAgentPromptRunnerOptions{
        .executor = strand,
        .assembly = &*assembly,
        .config = &cfg,
        .provider = system,
        .route = route,
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

  if (channels_enabled) {
    // Construct the configured adapters into a strand-owned manager (the pumps
    // and dispatcher run on `strand`). register_configured_channels is
    // construction-only: serve_body starts the adapters and serve_channels
    // drives receive/dispatch.
    channel_manager.emplace(strand);
    auto report = register_configured_channels(*channel_manager, strand, cfg);
    if (!report) {
      return std::unexpected(std::move(report).error());
    }
    for (const auto& skipped : report->skipped) {
      std::println(stderr,
                   "orangutan: channel '{}' (kind '{}') has no adapter in this build; skipping",
                   skipped.id,
                   skipped.kind);
    }
    // Drive only the adapters that actually registered (unknown/disabled kinds
    // were skipped above).
    for (const auto& configured : cfg.channels()) {
      if (channel_manager->contains(configured.id)) {
        channel_ids.push_back(configured.id);
      }
    }
    if (!channel_ids.empty()) {
      auto routed = make_routed_channel_prompt_runner(ChannelAgentPromptRunnerOptions{
          .executor = strand,
          .assembly = &*assembly,
          .config = &cfg,
          .provider = system,
          .route = route,
          .max_tokens = 1024,
      });
      if (!routed) {
        return std::unexpected(std::move(routed).error());
      }
      channel_runner = std::move(*routed);
    }
  }

  // Serve channels only when at least one configured adapter registered in this
  // build (e.g. a `qq`-only config in a non-`--channel_qq` build registers none).
  const bool serve_channels_enabled = !channel_ids.empty();
  const auto channel_count = channel_ids.size();
  auto channel_options = ServeChannelOptions{};
  if (serve_channels_enabled) {
    channel_options.metrics_observer = ServeChannelMetricsLogSink{};
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
                            std::move(*triggered_seeds),
                            std::move(cron_handler),
                            std::move(triggered_handler),
                            ServeAutomationOptions{},
                            automation_enabled ? &*shared_scheduler : nullptr,
                            ServeSchedulerReapOptions{},
                            webhooks_enabled,
                            std::move(webhook_options),
                            serve_channels_enabled,
                            channel_manager ? &*channel_manager : nullptr,
                            std::move(channel_runner),
                            std::move(channel_ids),
                            std::move(channel_options),
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
    std::println("  automation: {} ({} cron job(s), {} triggered job(s), {} provider)",
                 automation_db,
                 cron_job_count,
                 triggered_job_count,
                 provider_live ? "live" : "offline");
    std::println("  scheduler:  idle-lock reaping every {}s",
                 std::chrono::duration_cast<std::chrono::seconds>(ServeSchedulerReapOptions{}.reap_interval).count());
  }
  if (webhooks_enabled) {
    const auto& listener = cfg.automation().webhooks.listener;
    std::println("  webhooks:   http://{}:{}{}<id>", listener.bind_host, listener.port, listener.path_prefix);
  }
  if (serve_channels_enabled) {
    std::println("  channels:   {} adapter(s), {} provider", channel_count, provider_live ? "live" : "offline");
    std::println("  channel metrics: stderr snapshots");
  }
  std::println("  stop with Ctrl-C (SIGINT) or SIGTERM");

  auto result = service_future.get();  // blocks until a signal stops the service
  auto stopped = runtime.stop_and_join();
  if (!stopped) {
    return std::unexpected(std::move(stopped).error());
  }

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
