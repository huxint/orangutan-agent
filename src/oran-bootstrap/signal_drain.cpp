// src/oran-bootstrap/signal_drain.cpp — signal-aware io_context drain.

#include <oran/bootstrap/signal_drain.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <csignal>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>

#include <oran/core/error.hpp>

namespace orangutan::bootstrap {

struct SignalScope::Impl {
  Impl(asio::io_context& io) : signals(io, SIGINT, SIGTERM), io_ref(io) {}

  asio::signal_set signals;
  asio::io_context& io_ref;
  std::atomic<int> signum{0};
};

SignalScope::SignalScope(asio::io_context& io) : impl_(std::make_unique<Impl>(io)) {
  impl_->signals.async_wait([impl_ptr = impl_.get()](const std::error_code& ec, int sig) {
    if (ec) {
      return;
    }
    impl_ptr->signum.store(sig, std::memory_order_release);
    impl_ptr->io_ref.stop();
  });
}

SignalScope::~SignalScope() = default;

void SignalScope::release() noexcept {
  if (!impl_) {
    return;
  }
  std::error_code ec;
  impl_->signals.cancel(ec);
}

int SignalScope::signum() const noexcept {
  return impl_ ? impl_->signum.load(std::memory_order_acquire) : 0;
}

std::string_view signal_name(int signum) noexcept {
  switch (signum) {
    case SIGINT:
      return "SIGINT";
    case SIGTERM:
      return "SIGTERM";
    default:
      return "unknown";
  }
}

std::optional<int> signum_from_error(const core::Error& error) noexcept {
  if (error.kind() != core::ErrorKind::cancelled) {
    return std::nullopt;
  }
  const auto ctx = error.context();
  const auto it = std::ranges::find_if(ctx, [](const auto& kv) { return kv.first == "signum"; });
  if (it == ctx.end()) {
    return std::nullopt;
  }
  const auto& value = it->second;
  int signum = 0;
  const auto* first = value.data();
  const auto* last = value.data() + value.size();
  const auto parse = std::from_chars(first, last, signum);
  if (parse.ec != std::errc{} || parse.ptr != last || signum <= 0) {
    return std::nullopt;
  }
  return signum;
}

}  // namespace orangutan::bootstrap
