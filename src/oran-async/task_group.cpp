// src/oran-async/task_group.cpp — bounded ownership for named child tasks.

#include <oran/async/task_group.hpp>

#include <algorithm>
#include <exception>
#include <expected>
#include <mutex>
#include <ranges>
#include <utility>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/cancellation_type.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/error.hpp>
#include <asio/post.hpp>
#include <asio/strand.hpp>
#include <asio/system_error.hpp>
#include <asio/this_coro.hpp>

#include <oran/async/channel.hpp>
#include <oran/core/error.hpp>

namespace orangutan::async {

namespace {

struct ChildRecord {
  std::string name;
  std::shared_ptr<asio::cancellation_signal> cancellation;
  std::optional<TaskOutcome> outcome{};
};

[[nodiscard]] TaskOutcome failed_outcome(std::string name, core::Error error) {
  const auto status =
      error.kind() == core::ErrorKind::cancelled ? TaskOutcomeStatus::cancelled : TaskOutcomeStatus::failed;
  return TaskOutcome{.name = std::move(name), .status = status, .error = std::move(error)};
}

}  // namespace

struct TaskGroup::Impl : std::enable_shared_from_this<TaskGroup::Impl> {
  Impl(asio::any_io_executor exec, TaskGroupOptions opts)
      : executor{asio::make_strand(std::move(exec))}, options{opts}, completion{executor, 1} {}

  asio::any_io_executor executor;
  TaskGroupOptions options;
  Channel<std::monostate> completion;
  mutable std::mutex mutex;
  std::vector<ChildRecord> children;
  std::size_t active{};
  bool closed{false};
  bool stop_requested{false};
  bool join_started{false};

  [[nodiscard]] core::Result<void> spawn(std::string name, Task task) {
    if (name.empty()) {
      return std::unexpected(core::Error::invalid_argument("task group child name must be non-empty"));
    }
    if (!task) {
      return std::unexpected(core::Error::invalid_argument("task group child task is empty"));
    }

    std::size_t index{};
    auto cancellation = std::make_shared<asio::cancellation_signal>();
    {
      const std::scoped_lock lock{mutex};
      if (closed) {
        return std::unexpected(core::Error{core::ErrorKind::conflict, "task group is closed"});
      }
      if (active >= options.max_tasks) {
        return std::unexpected(core::Error{core::ErrorKind::mailbox_overflowed, "task group capacity reached"}.with(
            "max_tasks",
            std::to_string(options.max_tasks)));
      }
      index = children.size();
      children.push_back(ChildRecord{.name = std::move(name), .cancellation = cancellation});
      ++active;
    }

    auto self = shared_from_this();
    try {
      asio::co_spawn(executor,
                     run_child(std::move(self), index, std::move(task)),
                     asio::bind_cancellation_slot(cancellation->slot(), asio::detached));
    } catch (const std::exception& error) {
      finish(index,
             failed_outcome(child_name(index),
                            core::Error::internal("task group failed to spawn child").with("reason", error.what())));
    } catch (...) {
      finish(index,
             failed_outcome(child_name(index),
                            core::Error::internal("task group failed to spawn child").with("reason", "unknown")));
    }
    return {};
  }

  void close() noexcept {
    const std::scoped_lock lock{mutex};
    closed = true;
  }

  void request_stop() noexcept {
    auto signals = std::vector<std::shared_ptr<asio::cancellation_signal>>{};
    {
      const std::scoped_lock lock{mutex};
      closed = true;
      if (stop_requested) {
        return;
      }
      stop_requested = true;
      signals.reserve(active);
      for (const auto& child : children) {
        if (!child.outcome.has_value()) {
          signals.push_back(child.cancellation);
        }
      }
    }

    for (auto& signal : signals) {
      asio::post(executor, [signal = std::move(signal)] { signal->emit(asio::cancellation_type::all); });
    }
  }

  [[nodiscard]] std::size_t active_tasks() const noexcept {
    const std::scoped_lock lock{mutex};
    return active;
  }

  [[nodiscard]] core::Result<void> begin_join() {
    const std::scoped_lock lock{mutex};
    closed = true;
    if (join_started) {
      return std::unexpected(core::Error{core::ErrorKind::conflict, "task group join already started"});
    }
    join_started = true;
    return {};
  }

  [[nodiscard]] TaskGroupReport report() const {
    const std::scoped_lock lock{mutex};
    auto report = TaskGroupReport{};
    report.tasks.reserve(children.size());
    for (const auto& child : children) {
      if (child.outcome.has_value()) {
        report.tasks.push_back(*child.outcome);
      }
    }
    return report;
  }

private:
  [[nodiscard]] std::string child_name(std::size_t index) const {
    const std::scoped_lock lock{mutex};
    return children[index].name;
  }

  void finish(std::size_t index, TaskOutcome outcome) {
    {
      const std::scoped_lock lock{mutex};
      auto& child = children[index];
      if (child.outcome.has_value()) {
        return;
      }
      child.outcome = std::move(outcome);
      --active;
    }
    static_cast<void>(completion.try_send(std::monostate{}));
  }

  [[nodiscard]] static Awaitable<void> run_child(std::shared_ptr<Impl> state, std::size_t index, Task task) {
    auto outcome = TaskOutcome{.name = state->child_name(index)};
    try {
      auto result = co_await task();
      if (!result) {
        outcome = failed_outcome(std::move(outcome.name), std::move(result).error());
      }
    } catch (const asio::system_error& error) {
      if (error.code() == asio::error::operation_aborted) {
        outcome = failed_outcome(std::move(outcome.name), core::Error::cancelled());
      } else {
        outcome = failed_outcome(std::move(outcome.name),
                                 core::Error::internal("task group child threw asio error")
                                     .with("reason", error.what())
                                     .with("asio_error", error.code().message()));
      }
    } catch (const std::exception& error) {
      outcome = failed_outcome(std::move(outcome.name),
                               core::Error::internal("task group child threw").with("reason", error.what()));
    } catch (...) {
      outcome = failed_outcome(std::move(outcome.name),
                               core::Error::internal("task group child threw").with("reason", "unknown"));
    }

    state->finish(index, std::move(outcome));
  }
};

std::size_t TaskGroupReport::succeeded() const noexcept {
  return static_cast<std::size_t>(std::ranges::count(tasks, TaskOutcomeStatus::succeeded, &TaskOutcome::status));
}

std::size_t TaskGroupReport::failed() const noexcept {
  return static_cast<std::size_t>(std::ranges::count(tasks, TaskOutcomeStatus::failed, &TaskOutcome::status));
}

std::size_t TaskGroupReport::cancelled() const noexcept {
  return static_cast<std::size_t>(std::ranges::count(tasks, TaskOutcomeStatus::cancelled, &TaskOutcome::status));
}

bool TaskGroupReport::all_succeeded() const noexcept {
  return failed() == 0 && cancelled() == 0;
}

core::Result<TaskGroup> TaskGroup::create(asio::any_io_executor executor, TaskGroupOptions options) {
  if (options.max_tasks == 0) {
    return std::unexpected(core::Error::invalid_argument("task group max_tasks must be positive"));
  }
  return TaskGroup{std::make_shared<Impl>(std::move(executor), options)};
}

TaskGroup::TaskGroup(std::shared_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}

TaskGroup::~TaskGroup() {
  if (impl_) {
    impl_->request_stop();
  }
}

TaskGroup::TaskGroup(TaskGroup&&) noexcept = default;

TaskGroup& TaskGroup::operator=(TaskGroup&& other) noexcept {
  if (this != &other) {
    if (impl_) {
      impl_->request_stop();
    }
    impl_ = std::move(other.impl_);
  }
  return *this;
}

core::Result<void> TaskGroup::spawn(std::string name, Task task) {
  if (!impl_) {
    return std::unexpected(core::Error{core::ErrorKind::conflict, "task group has been moved from"});
  }
  return impl_->spawn(std::move(name), std::move(task));
}

void TaskGroup::close() noexcept {
  if (impl_) {
    impl_->close();
  }
}

void TaskGroup::request_stop() noexcept {
  if (impl_) {
    impl_->request_stop();
  }
}

Awaitable<core::Result<TaskGroupReport>> TaskGroup::join() {
  co_await asio::this_coro::throw_if_cancelled(false);
  if (!impl_) {
    co_return std::unexpected(core::Error{core::ErrorKind::conflict, "task group has been moved from"});
  }
  if (auto begun = impl_->begin_join(); !begun) {
    co_return std::unexpected(std::move(begun).error());
  }

  while (impl_->active_tasks() != 0) {
    auto completed = co_await impl_->completion.receive();
    if (!completed) {
      impl_->request_stop();
      co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
    }
  }
  co_return impl_->report();
}

std::size_t TaskGroup::active_tasks() const noexcept {
  return impl_ ? impl_->active_tasks() : 0;
}

TaskGroupOptions TaskGroup::options() const noexcept {
  return impl_ ? impl_->options : TaskGroupOptions{};
}

}  // namespace orangutan::async
