// include/oran/async/runtime.hpp — bootstrap-owned asio runtime.

#pragma once

#include <cstddef>
#include <memory>

#include <asio/any_io_executor.hpp>
#include <asio/strand.hpp>

#include <oran/core/result.hpp>

namespace orangutan::async {

struct RuntimeConfig {
  std::size_t io_workers{1};
  std::size_t cpu_workers{1};
};

class Runtime {
public:
  explicit Runtime(RuntimeConfig config = RuntimeConfig{});
  ~Runtime();

  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  Runtime(Runtime&&) noexcept;
  Runtime& operator=(Runtime&&) noexcept;

  [[nodiscard]] asio::any_io_executor executor() const noexcept;
  [[nodiscard]] asio::any_io_executor cpu_executor() const noexcept;
  [[nodiscard]] asio::strand<asio::any_io_executor> make_strand() const;

  [[nodiscard]] core::Result<void> run();

  /// Spawn the io workers and return immediately, leaving the runtime running
  /// on its own threads. Use this when another event loop owns the calling
  /// thread (e.g. the Slint desktop UI): `start()` then drive the UI, and
  /// `stop()` on teardown. Returns a `conflict` error if already running or
  /// stopped. Unlike `run()`, errors raised by a worker after `start()` are not
  /// surfaced here; detached coroutines must catch their own (async A8).
  [[nodiscard]] core::Result<void> start();

  void stop() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace orangutan::async
