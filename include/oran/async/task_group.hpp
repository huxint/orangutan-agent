// include/oran/async/task_group.hpp — bounded ownership for named child tasks.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <asio/any_io_executor.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>

namespace orangutan::async {

struct TaskGroupOptions {
  std::size_t max_tasks{64};

  friend bool operator==(const TaskGroupOptions&, const TaskGroupOptions&) = default;
};

enum class TaskOutcomeStatus : std::uint8_t {
  succeeded,
  failed,
  cancelled,
};

struct TaskOutcome {
  std::string name;
  TaskOutcomeStatus status{TaskOutcomeStatus::succeeded};
  std::optional<core::Error> error{};

  friend bool operator==(const TaskOutcome&, const TaskOutcome&) = default;
};

struct TaskGroupReport {
  std::vector<TaskOutcome> tasks;

  [[nodiscard]] std::size_t succeeded() const noexcept;
  [[nodiscard]] std::size_t failed() const noexcept;
  [[nodiscard]] std::size_t cancelled() const noexcept;
  [[nodiscard]] bool all_succeeded() const noexcept;
};

/// A bounded set of named child coroutines. `spawn` accepts a type-erased
/// factory so coroutine implementation details stay in the owning library's
/// `.cpp`; the public surface does not instantiate a task-set template per
/// caller.
///
/// `close()` rejects future children. `request_stop()` closes the group and
/// emits cancellation to every active child. `join()` closes the group, waits
/// for every spawned child, and returns one outcome in spawn order. Child
/// failures are report rows rather than a failed `join()` result.
///
/// Destruction requests cancellation but cannot asynchronously join. Every
/// spawned child retains the group state until it records its outcome, so no
/// detached coroutine can access destroyed task-group state. Owners that need
/// all borrowed application state released before teardown must `co_await
/// join()` explicitly.
class TaskGroup {
public:
  using Task = std::move_only_function<Awaitable<core::Result<void>>()>;

  [[nodiscard]] static core::Result<TaskGroup> create(asio::any_io_executor executor, TaskGroupOptions options = {});

  ~TaskGroup();

  TaskGroup(const TaskGroup&) = delete;
  TaskGroup& operator=(const TaskGroup&) = delete;
  TaskGroup(TaskGroup&&) noexcept;
  TaskGroup& operator=(TaskGroup&&) noexcept;

  [[nodiscard]] core::Result<void> spawn(std::string name, Task task);
  void close() noexcept;
  void request_stop() noexcept;
  [[nodiscard]] Awaitable<core::Result<TaskGroupReport>> join();

  [[nodiscard]] std::size_t active_tasks() const noexcept;
  [[nodiscard]] TaskGroupOptions options() const noexcept;

private:
  struct Impl;
  explicit TaskGroup(std::shared_ptr<Impl> impl) noexcept;

  std::shared_ptr<Impl> impl_;
};

}  // namespace orangutan::async
