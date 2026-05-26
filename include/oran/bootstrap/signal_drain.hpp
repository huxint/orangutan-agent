// include/oran/bootstrap/signal_drain.hpp — signal-aware io_context drain.
//
// Slice 23 introduces the first concrete cancel-aware drain in the
// repository: a RAII scope that installs `asio::signal_set` handlers for
// SIGINT and SIGTERM on an `asio::io_context`. While the scope is alive,
// the caller drives `io.run()`; when a signal arrives the scope calls
// `io.stop()` and records the POSIX signum. The caller is responsible
// for calling `release()` once its real work has finished — that cancels
// the pending `async_wait` so `io.run()` can return naturally when no
// other work is pending.
//
// Why the explicit `release()`. `asio::signal_set::async_wait` is itself
// pending work on the io_context. Without an explicit cancel, `io.run()`
// blocks forever waiting for a signal that never arrives. The
// post-work-completion `release()` call is the simplest way to express
// "we're done watching for signals" without introducing a separate
// completion sentinel.
//
// Cancellation surface (per `docs/rules/critical-rules.md#C11`): the
// caller's coroutines do not have to be cancel-aware for the scope to
// work; `io.stop()` is a blunt drop that the one-shot `--audit-init`
// drain can rely on because SQLite WAL commits atomically. The
// agent-loop drain (a future slice) will replace `io.stop()` with a
// fine-grained `asio::cancellation_signal` once the loop's
// cancellation-state plumbing lands.

#pragma once

#include <memory>
#include <optional>
#include <string_view>

namespace asio {
class io_context;
}  // namespace asio

namespace orangutan::core {
class Error;
}  // namespace orangutan::core

namespace orangutan::bootstrap {

/// RAII trap for SIGINT and SIGTERM on a single `asio::io_context`.
/// Constructing the scope installs `asio::signal_set` handlers; the
/// scope is non-copyable and non-movable so the embedded signal_set
/// stays bound to its `io_context`.
class SignalScope {
public:
  /// Install SIGINT + SIGTERM handlers on `io`.
  explicit SignalScope(asio::io_context& io);
  ~SignalScope();

  SignalScope(const SignalScope&) = delete;
  SignalScope& operator=(const SignalScope&) = delete;
  SignalScope(SignalScope&&) = delete;
  SignalScope& operator=(SignalScope&&) = delete;

  /// Cancel the pending `async_wait` so `io.run()` can return once no
  /// other work is pending. Idempotent; once a real signal has fired
  /// the scope has already stopped the `io_context`, so the additional
  /// cancel is a no-op.
  void release() noexcept;

  /// The POSIX signum that fired, or `0` if none has fired yet. The
  /// value is set from the signal handler and read after `io.run()`
  /// returns, so the read race only matters when the caller forgets
  /// to drain the executor before inspecting the scope.
  [[nodiscard]] int signum() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Stable spelling for the signum the drain caught. Returns `"SIGINT"`,
/// `"SIGTERM"`, or `"unknown"` so error context entries stay stable
/// across log/redaction passes; the raw integer travels alongside in a
/// separate context entry.
[[nodiscard]] std::string_view signal_name(int signum) noexcept;

/// Recover the POSIX signum carried on a `cancelled` error produced by
/// `bootstrap::run` after a SIGINT/SIGTERM trap. The cancelled error
/// carries `signal` (matching `signal_name`) and `signum` context
/// entries, and this helper parses the latter. Returns `std::nullopt`
/// when the error is not cancelled, when the `signum` context entry is
/// absent, or when it does not parse as a positive integer. The helper
/// exists so callers (including `bootstrap::run`) translate signal-driven
/// cancellation into shell-conventional exit codes (128 + signum)
/// without poking at `Error::context()` directly.
[[nodiscard]] std::optional<int> signum_from_error(const core::Error& error) noexcept;

}  // namespace orangutan::bootstrap
