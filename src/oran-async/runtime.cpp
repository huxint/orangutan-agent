// src/oran-async/runtime.cpp — Runtime implementation.

#include <oran/async/runtime.hpp>

#include <algorithm>
#include <atomic>
#include <exception>
#include <expected>
#include <mutex>
#include <optional>
#include <string>

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/thread_pool.hpp>

#include <oran/core/error.hpp>

namespace orangutan::async {

namespace {

[[nodiscard]] RuntimeConfig normalize(RuntimeConfig config) noexcept {
  config.io_workers = std::max<std::size_t>(1, config.io_workers);
  config.cpu_workers = std::max<std::size_t>(1, config.cpu_workers);
  return config;
}

enum class RunState {
  idle,
  running,
  stopped,
};

}  // namespace

struct Runtime::Impl {
  explicit Impl(RuntimeConfig c)
      : config(normalize(c)), io_context(static_cast<int>(config.io_workers)),
        work_guard(asio::make_work_guard(io_context)), io_workers(config.io_workers), cpu_workers(config.cpu_workers) {}

  ~Impl() {
    stop();
  }

  RuntimeConfig config;
  asio::io_context io_context;
  asio::executor_work_guard<asio::io_context::executor_type> work_guard;
  asio::thread_pool io_workers;
  asio::thread_pool cpu_workers;
  std::atomic<RunState> state{RunState::idle};
  std::mutex run_error_mutex;
  std::optional<core::Error> run_error;

  [[nodiscard]] core::Result<void> run() {
    auto expected = RunState::idle;
    if (!state.compare_exchange_strong(expected, RunState::running)) {
      const auto message = expected == RunState::running ? "runtime is already running" : "runtime has already stopped";
      return std::unexpected(core::Error{core::ErrorKind::conflict, message});
    }

    for (std::size_t i = 0; i < config.io_workers; ++i) {
      asio::post(io_workers, [this] { run_io_context_worker(); });
    }

    io_workers.join();
    cpu_workers.stop();
    cpu_workers.join();
    if (auto error = take_run_error(); error) {
      return std::unexpected(std::move(*error));
    }
    return {};
  }

  void stop() noexcept {
    if (state.exchange(RunState::stopped) == RunState::stopped) {
      return;
    }
    work_guard.reset();
    io_context.stop();
    cpu_workers.stop();
  }

private:
  void run_io_context_worker() {
    try {
      io_context.run();
    } catch (const std::exception& error) {
      record_run_error(core::Error::internal("runtime io worker failed").with("reason", error.what()));
      stop();
    } catch (...) {
      record_run_error(core::Error::internal("runtime io worker failed").with("reason", "unknown"));
      stop();
    }
  }

  void record_run_error(core::Error error) {
    const std::scoped_lock lock{run_error_mutex};
    if (!run_error) {
      run_error = std::move(error);
    }
  }

  [[nodiscard]] std::optional<core::Error> take_run_error() {
    const std::scoped_lock lock{run_error_mutex};
    return std::move(run_error);
  }
};

Runtime::Runtime(RuntimeConfig config) : impl_(std::make_unique<Impl>(config)) {}

Runtime::~Runtime() = default;

Runtime::Runtime(Runtime&&) noexcept = default;

Runtime& Runtime::operator=(Runtime&&) noexcept = default;

asio::any_io_executor Runtime::executor() const noexcept {
  return impl_->io_context.get_executor();
}

asio::any_io_executor Runtime::cpu_executor() const noexcept {
  return impl_->cpu_workers.get_executor();
}

asio::strand<asio::any_io_executor> Runtime::make_strand() const {
  return asio::make_strand(executor());
}

core::Result<void> Runtime::run() {
  return impl_->run();
}

void Runtime::stop() noexcept {
  impl_->stop();
}

}  // namespace orangutan::async
