// include/oran/core/result.hpp — `Result<T>` and `all_ok`.
//
// Slice-0 public surface. The alias keeps the legacy `utils::all_ok` pattern
// (docs/rules/error-handling.md) so multi-step initialization can short-circuit
// on the first failure without per-call `if (!r) return std::unexpected(...)`.

#pragma once

#include <expected>
#include <tuple>
#include <type_traits>
#include <utility>

#include <oran/core/error.hpp>

namespace orangutan::core {

template <typename T>
using Result = std::expected<T, Error>;

namespace detail {

template <typename T>
struct is_result : std::false_type {};
template <typename T>
struct is_result<Result<T>> : std::true_type {};
template <typename T>
inline constexpr bool is_result_v = is_result<std::remove_cvref_t<T>>::value;

template <typename T>
using result_value_t = typename std::remove_cvref_t<T>::value_type;

}  // namespace detail

/// Combine N `Result<T_i>` into a single `Result<std::tuple<T_i...>>`. Returns
/// the first error encountered, in argument order — later arguments are *not*
/// inspected (no extra work past the first failure).
///
/// Example:
///   auto r = all_ok(parse_a(...), parse_b(...), parse_c(...));
///   if (!r) return std::unexpected(r.error());
///   auto [a, b, c] = *std::move(r);
template <typename... Rs>
  requires(detail::is_result_v<Rs> && ...)
[[nodiscard]] auto all_ok(Rs&&... results) -> Result<std::tuple<detail::result_value_t<Rs>...>> {
  using Tuple = std::tuple<detail::result_value_t<Rs>...>;

  Error error{ErrorKind::ok, {}};
  bool failed = false;

  auto inspect = [&]<typename R>(R&& r) {
    if (!failed && !r.has_value()) {
      failed = true;
      error = std::forward<R>(r).error();
    }
  };
  (inspect(results), ...);

  if (failed) {
    return std::unexpected(std::move(error));
  }
  return Tuple{std::forward<Rs>(results).value()...};
}

}  // namespace orangutan::core
