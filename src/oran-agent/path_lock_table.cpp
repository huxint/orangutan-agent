// src/oran-agent/path_lock_table.cpp — per-path read/write lock table.

#include "_impl/path_lock_table.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <asio/any_io_executor.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/async/channel.hpp>
#include <oran/core/error.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>

namespace orangutan::agent::detail {

PathLockGuard::PathLockGuard(PathLockTable* table, std::string path, PathLockMode mode) noexcept
    : table_{table}, path_{std::move(path)}, mode_{mode}, held_{true} {}

PathLockGuard::PathLockGuard(PathLockGuard&& other) noexcept
    : table_{other.table_}, path_{std::move(other.path_)}, mode_{other.mode_}, held_{other.held_} {
  other.held_ = false;
  other.table_ = nullptr;
}

PathLockGuard& PathLockGuard::operator=(PathLockGuard&& other) noexcept {
  if (this != &other) {
    reset();
    table_ = other.table_;
    path_ = std::move(other.path_);
    mode_ = other.mode_;
    held_ = other.held_;
    other.held_ = false;
    other.table_ = nullptr;
  }
  return *this;
}

PathLockGuard::~PathLockGuard() {
  reset();
}

void PathLockGuard::reset() noexcept {
  if (held_ && table_ != nullptr) {
    table_->release(path_, mode_);
  }
  held_ = false;
  table_ = nullptr;
}

PathLockTable::PathLockTable(PathLockTableOptions options) noexcept : options_{options} {}

async::Awaitable<core::Result<PathLockGuard>>
PathLockTable::acquire(asio::any_io_executor exec, std::string path, PathLockMode mode, core::Time now) {
  // Phase 1: synchronous decision — either acquire immediately or queue a
  // waiter. The map iterator may be invalidated by later inserts, so capture
  // any references before suspending.
  std::shared_ptr<async::Channel<std::monostate>> wake;
  std::uint64_t my_id = 0;

  {
    const auto [it, inserted] = entries_.try_emplace(path);
    Entry& entry = it->second;
    if (inserted) {
      stats_.current_entries = entries_.size();
      stats_.peak_entries = std::max(stats_.peak_entries, stats_.current_entries);
    }

    const auto can_take_now = [&] {
      if (!entry.waiters.empty()) {
        return false;
      }
      if (mode == PathLockMode::shared) {
        return !entry.writer;
      }
      return entry.readers == 0 && !entry.writer;
    }();

    if (can_take_now) {
      if (mode == PathLockMode::shared) {
        ++entry.readers;
        ++stats_.shared_acquires;
      } else {
        entry.writer = true;
        ++stats_.exclusive_acquires;
      }
      update_idle_locked(entry, now);
      co_return PathLockGuard{this, std::move(path), mode};
    }

    ++stats_.contended_acquires;
    wake = std::make_shared<async::Channel<std::monostate>>(exec, /*capacity=*/1);
    my_id = next_waiter_id_++;
    entry.waiters.push_back(Waiter{.id = my_id, .mode = mode, .wake = wake});
    update_idle_locked(entry, now);
  }

  // Phase 2: wait for a wake permit. The channel is single-use; either we
  // receive the permit (lock granted on our behalf by `wake_next_locked`) or
  // the receive cancels.
  auto receive_result = co_await wake->receive();

  // Phase 3: re-acquire entry and reconcile. The map state may have changed
  // (e.g., reap), but a live waiter holds a strong reference via the channel,
  // so the corresponding entry must still exist — `reap` skips entries with
  // outstanding waiters.
  auto it = entries_.find(path);
  if (it == entries_.end()) {
    // Should not happen: reap preserves entries with waiters. Treat as a
    // hard error so a regression is loud.
    ++stats_.cancelled_acquires;
    co_return std::unexpected(core::Error::internal("path_lock_table: entry vanished while waiting"));
  }
  Entry& entry = it->second;

  if (!receive_result.has_value()) {
    // Cancellation. Two cases:
    //   (a) We are still in the waiters queue — release_locked never woke
    //       us. Just erase ourselves.
    //   (b) wake_next_locked already pre-incremented the counter on our
    //       behalf and removed us from the queue, but our receive cancelled
    //       before consuming the permit. Decrement the counter we never
    //       actually claimed and forward the wake to the next waiter so the
    //       chain does not stall.
    const auto erased = std::erase_if(entry.waiters, [my_id](const Waiter& w) { return w.id == my_id; });
    if (erased == 0) {
      if (mode == PathLockMode::shared) {
        if (entry.readers > 0) {
          --entry.readers;
        }
      } else {
        entry.writer = false;
      }
      wake_next_locked(entry);
    }
    update_idle_locked(entry, now);
    ++stats_.cancelled_acquires;
    co_return std::unexpected(std::move(receive_result).error());
  }

  // Permit received: the counter was already incremented by
  // `wake_next_locked` and we were popped from the queue.
  if (mode == PathLockMode::shared) {
    ++stats_.shared_acquires;
  } else {
    ++stats_.exclusive_acquires;
  }
  update_idle_locked(entry, now);
  co_return PathLockGuard{this, std::move(path), mode};
}

void PathLockTable::release(std::string_view path, PathLockMode mode) {
  auto it = entries_.find(path);
  if (it == entries_.end()) {
    return;
  }
  Entry& entry = it->second;

  if (mode == PathLockMode::shared) {
    if (entry.readers > 0) {
      --entry.readers;
    }
  } else {
    entry.writer = false;
  }
  wake_next_locked(entry);
  update_idle_locked(entry, core::time::now_utc());
}

std::size_t PathLockTable::reap(core::Time now) {
  if (options_.idle_ttl == std::chrono::milliseconds{0}) {
    return 0;
  }
  const auto deadline = now.to_system_time_point() - options_.idle_ttl;
  std::size_t evicted = 0;
  for (auto it = entries_.begin(); it != entries_.end();) {
    const Entry& entry = it->second;
    const bool is_idle = entry.readers == 0 && !entry.writer && entry.waiters.empty() && entry.idle_since.has_value();
    if (is_idle && entry.idle_since->to_system_time_point() <= deadline) {
      it = entries_.erase(it);
      ++evicted;
    } else {
      ++it;
    }
  }
  stats_.current_entries = entries_.size();
  stats_.reaped_entries += evicted;
  return evicted;
}

void PathLockTable::wake_next_locked(Entry& entry) {
  while (!entry.waiters.empty()) {
    const auto& front = entry.waiters.front();
    const bool can_wake =
        (front.mode == PathLockMode::exclusive) ? (entry.readers == 0 && !entry.writer) : !entry.writer;
    if (!can_wake) {
      break;
    }

    // Copy the fields we need before popping; pop invalidates the reference.
    const auto mode = front.mode;
    auto channel = front.wake;
    entry.waiters.pop_front();

    if (mode == PathLockMode::shared) {
      ++entry.readers;
    } else {
      entry.writer = true;
    }

    // The channel is sized 1 and never sent to before this point, so
    // `try_send` cannot back-pressure. A cancelled waiter's channel still
    // accepts the permit; the cancellation arm in `acquire` reconciles.
    [[maybe_unused]] auto sent = channel->try_send(std::monostate{});

    if (mode == PathLockMode::exclusive) {
      break;
    }
    // Shared wake fans out: continue waking consecutive shared waiters until
    // we hit an exclusive request or the queue empties.
  }
}

void PathLockTable::update_idle_locked(Entry& entry, core::Time now) noexcept {
  if (entry.readers == 0 && !entry.writer && entry.waiters.empty()) {
    if (!entry.idle_since.has_value()) {
      entry.idle_since = now;
    }
  } else {
    entry.idle_since.reset();
  }
}

}  // namespace orangutan::agent::detail
