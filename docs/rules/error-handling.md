# Error Handling

Fallible library boundaries return `core::Result<T>`, an alias for
`std::expected<T, core::Error>`. The declarations in
[`error.hpp`](../../include/oran/core/error.hpp) own error categories, builders,
context and retry metadata; [`result.hpp`](../../include/oran/core/result.hpp)
owns the alias.

## Sequential Work

Check a result before starting the next dependent operation. This preserves
execution order and avoids effects after failure:

```cpp
core::Result<Combined> combine_inputs(Input input) {
  auto first = parse_first(input);
  if (!first) {
    return std::unexpected(std::move(first).error());
  }
  auto second = parse_second(input, *first);
  if (!second) {
    return std::unexpected(std::move(second).error());
  }
  return Combined{std::move(*first), std::move(*second)};
}
```

The unused `core::all_ok` helper is removed. It combined already evaluated
results; function arguments cannot short-circuit the operations producing them.
Use explicit checks when migrating callers. Pure synchronous transformations
may use standard `transform`, `and_then` and `or_else` operations:

```cpp
auto response = co_await provider.send(request, route);
auto usage = std::move(response).transform(
    [](provider::Response value) { return value.usage; });
```

These callbacks are synchronous. Await retries and other asynchronous work in
the surrounding coroutine, with explicit result checks.

## Boundary Rules

- No exceptions cross library boundaries. Catch exceptions from third-party or
  Asio operations at the owning boundary and translate them into the appropriate
  error category. Preserve cancellation as `ErrorKind::cancelled`; do not
  reclassify it as a network or storage failure.
- Public failure values use `Result<T>`; do not expose `std::exception_ptr` as a
  second error channel. Internal completion handlers may translate exceptions.
- Use `return` in ordinary functions and `co_return` in coroutines. Move owned
  errors when propagating them.
- Handle or propagate failed results; do not discard failures silently.
- Use `Result<std::optional<T>>` when absence is a normal outcome. An operation
  requiring a record may return `Error::not_found`; storage failure remains an
  error in either contract.
- Express recoverable invariant violations as `Error::internal`. Assertions may
  check programmer assumptions, but release behavior must not depend on them.
- Write explicit checks rather than control-flow macros, as required by C1.

Cancellation requests must be followed by the required resource join before
returning. [Async rules](async-and-concurrency.md) own coroutine lifetime and
cancellation details.

## Error Context And Retry

Keep categories machine-readable and attach diagnostic fields separately from
the message:

```cpp
auto response = co_await provider.send(request, route);
if (!response) {
  co_return std::unexpected(std::move(response).error()
                               .with("agent", agent_key)
                               .with("model", route.primary.model));
}
```

`Error::retryable()` classifies network, rate-limit, timeout and upstream errors.
It is a category predicate, not permission to retry an arbitrary effect. Respect
the operation's retry budget, `retry_after()` and visibility of partial output.
The [provider contract](../design-docs/api-portability.md) owns provider retries
and fallback. Never infer a category by parsing an error message.

Callers decide whether to report a returned error. `std::format` supports `Error`
and includes its category, message, context and retry delay. The runtime has no
logging facade; hosts own presentation. Do not include secrets in errors or
diagnostic output.

## Verification

Cover each fallible public API with a representative error case. Keep regressions
with their library's existing tests, including cancellation, denied effects and
storage integrity where relevant. Assign awaited results to locals before Catch2
assertions. [Testing and benchmarks](testing-and-bench.md) owns the verification
workflow; [critical rules](critical-rules.md) owns mandatory constraints.
