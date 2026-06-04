// include/oran/io/blocking.hpp — coroutine boundary for short blocking work.

#pragma once

#include <expected>
#include <type_traits>
#include <utility>

#include <asio/any_io_executor.hpp>
#include <asio/cancellation_type.hpp>
#include <asio/post.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/error.hpp>
#include <oran/core/result.hpp>

namespace orangutan::io {

/// Post a short, synchronous operation onto `executor` and return its
/// `core::Result<T>` through the coroutine boundary. Cancellation is checked
/// before and after the post; when already cancelled the callable is not
/// invoked and the helper returns `Error::cancelled`.
template <typename Fn>
  requires core::detail::is_result_v<std::invoke_result_t<Fn&>>
[[nodiscard]] async::Awaitable<std::invoke_result_t<Fn&>> run_blocking(asio::any_io_executor executor, Fn fn) {
  using ResultT = std::invoke_result_t<Fn&>;

  auto cancellation = co_await asio::this_coro::cancellation_state;
  if (cancellation.cancelled() != asio::cancellation_type::none) {
    co_return std::unexpected(core::Error::cancelled());
  }

  co_await asio::post(std::move(executor), asio::use_awaitable);
  if (cancellation.cancelled() != asio::cancellation_type::none) {
    co_return std::unexpected(core::Error::cancelled());
  }

  co_return ResultT{fn()};
}

}  // namespace orangutan::io
