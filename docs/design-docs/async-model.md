# Async Model

Orangutan v2 has **one** async vocabulary: standalone **asio** + **C++20 coroutines**.
Everything that crosses a yield point is an `awaitable<T>`. Everything is driven by a
single `asio::io_context` wrapped in `oran::async::Runtime`. There are no
`std::thread`s, no custom thread pools, no NVIDIA `stdexec` senders.

> The legacy `orangutan/` used a custom `stdexec` fork (NVIDIA gtc-2026). That fork
> existed in five files but transitively bled into `oran::provider`, `oran::automation`,
> and channel dispatch, becoming a top-tier compile-time tax. v2 chooses asio
> deliberately for its compile-time profile and ecosystem maturity.

> **Slice-1 status (2026-05-14):** `oran-async` ships `Runtime`,
> `Awaitable<T>`, bounded `Channel<T>`, and cancel-aware `sleep_for`. Mailbox
> policy and reusable async test helpers land in later slices. Bootstrap
> signal integration shipped in slice 23 as `bootstrap::SignalScope`
> (see [`Cancellation`](#cancellation) below).
> Slice 218 adds non-blocking `Channel<T>::try_receive()` for finite queue
> polling without requiring a waiter coroutine.

## Runtime Topology

```
                                              ┌──────────────────────────┐
oran::async::Runtime                          │  CPU pool (fixed)        │
 ├── asio::io_context (single instance)       │  size = config.runtime    │
 │     ├── thread #1                          │         .cpu_workers     │
 │     ├── thread #2                          │  for: memory distillation,│
 │     ├── …                                  │       large JSON parse,  │
 │     └── thread #N                          │       prompt rendering > │
 │   (size = RuntimeConfig.io_workers;        │             64 KiB       │
 │    config layer chooses deployment default)│                          │
 │                                            └──────────────────────────┘
 ├── stop_source (one shutdown signal)
 ├── steady_timer factory
 └── strand factory (per-agent serialization)
```

- **All I/O** (HTTP, SQLite via `oran-storage`'s WAL writer, file IO, subprocess) runs
  on the io_context's thread pool.
- **CPU-bound work** is dispatched to the separate `cpu_pool` via
  `co_await async::post(cpu_pool, ...)`.
- **Per-agent serialization** uses an `asio::strand` so that callbacks for the same
  agent never overlap, while different agents can run in parallel.
- **Bootstrap** instantiates exactly one `Runtime` and passes it by reference everywhere
  it's needed (see `docs/design-docs/module-boundaries.md`).

## Public Surface

```cpp
// include/oran/async/runtime.hpp — PUBLIC
namespace orangutan::async {

struct RuntimeConfig {
  std::size_t io_workers{1};
  std::size_t cpu_workers{1};
};

class Runtime {
 public:
  explicit Runtime(RuntimeConfig = {});
  ~Runtime();

  // Underlying executor. Library code accepts this by value (it's a handle).
  asio::any_io_executor executor() const noexcept;
  asio::any_io_executor cpu_executor() const noexcept;

  // Run the runtime once, until stop() is called or signal received.
  core::Result<void> run();
  // Spawn the io workers and return immediately, leaving the runtime running on
  // its own threads — for embedding under a foreign event loop (e.g. the Slint
  // desktop shell). Pair with stop() on teardown.
  core::Result<void> start();
  void stop() noexcept;
  core::Result<void> stop_and_join();

  // Strand factory: serializes work for one agent.
  asio::strand<asio::any_io_executor> make_strand() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace orangutan::async
```

The Runtime is owned by `oran-bootstrap`. Nothing else creates one.
Slice 1 normalizes zero worker counts to one. Slice 159 makes the `run()` lifecycle
explicit: a runtime may be run once, `stop()` transitions it to the stopped state,
and a later `run()` returns `ErrorKind::conflict` instead of trying to reuse an
already-stopped `asio::io_context`. Exceptions escaping an executor handler are
contained inside the IO worker, the runtime stops, and `run()` returns the first
failure as `ErrorKind::internal` with the thrown reason in structured context. The
later config/bootstrap slice decides the production default (for example
`min(8, hardware_concurrency)`) without making `oran-async` include `<thread>`.

Slice 251 adds `start()`: it spawns the io workers and returns immediately
(sharing the worker-spawn path with `run()`) so a foreign event loop — the Slint
desktop shell — can own the calling thread while the runtime drives its
coroutines on its own pool. It shares the same idle→running→stopped state
machine, so a second `start()` or a later `run()` returns `ErrorKind::conflict`,
and `stop()` plus destruction tear the pool down. Unlike `run()`, a worker
exception after `start()` is contained in the worker (the runtime stops) but is
surfaced by `stop_and_join()`. Start-mode owners call that boundary before
destroying any state borrowed by runtime tasks; `stop()` alone only requests
shutdown and does not establish a worker-lifetime join.

## Structured Child Ownership

`async::TaskGroup` is the default owner for sibling or background coroutines
whose lifetime can extend beyond one direct `co_await`. A group has explicit
`max_tasks` and bounded completed-outcome retention, accepts named
`Awaitable<Result<void>>` factories through `spawn`, and exposes `close`,
`request_stop`, `drain_completed`, and `join` boundaries. `join` closes the
group and waits for every accepted child, returning success/error/cancellation
outcomes in spawn order.

Destroying the facade requests cancellation but cannot perform an asynchronous
join. Each child retains the group implementation until it records its outcome,
which protects group bookkeeping; it does not make borrowed application state
safe. An owner whose tasks borrow sinks, services, providers, sockets, or other
runtime objects must explicitly `co_await join()` before releasing those
objects. Cancellation is cooperative, so `request_stop()` is not a substitute
for this ownership boundary.

The bounded webhook connection owner, advisory hook fan-out, `serve_channels`
adapter pumps, and the channel dispatcher's per-conversation workers use this
primitive. Further runtime-foundations work is migrating the remaining
hand-owned child sets (scheduler batches, desktop sessions) rather than adding
subsystem-local cancellation vectors and completion channels.

## Awaitable Alias

```cpp
// include/oran/async/awaitable_fwd.hpp
namespace orangutan::async {

template <typename T>
using Awaitable = asio::awaitable<T>;

}  // namespace orangutan::async
```

Every async function returns `Awaitable<core::Result<T>>`. Examples:

```cpp
Awaitable<core::Result<provider::Response>> provider::System::send(...) const;
Awaitable<core::Result<tool::Output>>       tool::Registry::dispatch(...) const;
Awaitable<core::Result<memory::Records>>    memory::Runtime::recall(...) const;
```

**Awaitables compose**; do not wrap them in `std::future` or custom promise types.

## Cancellation

Every public async function is cancel-aware. The pattern:

```cpp
Awaitable<core::Result<provider::Response>> send(provider::Request req) {
  auto cancel = co_await asio::this_coro::cancellation_state;
  if (cancel.cancelled() != asio::cancellation_type::none) {
    co_return core::Error::cancelled();
  }
  // ... actual work, periodically yielding so cancellation is delivered.
}
```

Subsystems that initiate cancellation:

- `oran-bootstrap` on SIGINT/SIGTERM: the slice-23 `bootstrap::SignalScope`
  RAII trap installs an `asio::signal_set` on a target `io_context` and
  calls `io.stop()` on first delivery. `--audit-init` adopts it for the
  one-shot drain; `bootstrap::run` translates the resulting
  `Error::cancelled` into shell-conventional `128 + signum` exit codes.
  The agent-loop slice will replace the blunt `io.stop()` with a
  fine-grained `asio::cancellation_signal` once the loop's
  cancel-state plumbing lands; the scope's header docstring tracks
  the deferred refinement.
- `oran-agent::Loop` on user `/cancel` or the desktop "stop" control: cancels the in-flight
  iteration.
- `oran-agent::ToolScheduler` (slice 116) on the agent loop's parent
  cancellation: the scheduler holds one `asio::cancellation_signal` per
  spawned tool call in a `std::deque<asio::cancellation_signal>` (the
  signal is neither copyable nor movable, so `deque`'s stable addresses
  are required) and emits on every child signal when its own
  `completion.receive()` resolves with `Error::cancelled`. It then disables
  its own cancellation filter
  (`asio::this_coro::reset_cancellation_state(asio::disable_cancellation())`)
  and waits out a 100 ms grace window (`kCancellationGrace`, spec 0012 AC5)
  for the children to wind down — a cancel-aware child resolves almost
  immediately, so the batch returns `Error::cancelled` with
  `reason=parent_cancelled` well inside the budget. A handler that ignores
  its cancellation slot cannot be forced to stop (asio cancellation is
  cooperative, and `co_await (dispatch || timeout)` does not resolve until
  that handler returns), so once the grace window expires the scheduler
  stops awaiting it: it records a `cancellation_lag` audit row naming the
  offending tool (slice 119) and returns, leaving the laggard to wind down
  on its own while the shared batch state keeps it alive. As of slice 120 the
  scheduler is the production tool-dispatch path: `agent::Loop` runs every
  batch (including N=1) through `run_batch`, and `bootstrap::AgentPromptRunner`
  owns a persistent one built from the `runtime.tool_scheduler.*` config block.
- `oran-orchestration` when a worker is stopped by a leader.
- `oran-automation` when a job is unscheduled mid-run.

Cancellation semantics are documented per subsystem in
`docs/design-docs/<subsystem>.md` under "Cancellation".

## Backpressure

Bounded queues are the default. The `oran::async::Channel<T>` type wraps a typed,
bounded, asio-aware queue:

```cpp
// include/oran/async/channel.hpp
namespace orangutan::async {

template <typename T>
class Channel {
 public:
  Channel(asio::any_io_executor, std::size_t capacity);

  // Returns Awaitable<Result<void>>; resolves when the item is enqueued.
  Awaitable<core::Result<void>> send(T value);

  // Non-blocking try_send: returns ErrorKind::mailbox_overflowed if full.
  core::Result<void> try_send(T value);

  // Non-blocking try_receive: returns nullopt when open and empty.
  core::Result<std::optional<T>> try_receive();

  // Returns Awaitable<Result<T>>; resolves when an item is available.
  Awaitable<core::Result<T>> receive();

  // Drains buffered values; pending/new operations complete with cancelled.
  void close() noexcept;
};

}  // namespace orangutan::async
```

Used by:

- `oran-orchestration::Mailbox` — bounded per agent.
- `oran-channel::ChannelManager::inbound_queue` — bounded per channel adapter.
- `oran-automation::JobQueue` — bounded; oldest pending job is dropped with a hook
  event when overflow occurs.
- `oran-automation::TriggeredQueue` — bounded process-local triggered job
  descriptors; callers can `try_receive()` or finite-drain available work
  without starting a detached queue loop.

`try_receive()` is a polling primitive, not a background consumer. It returns a
buffered value first, consumes a pending sender directly for zero-capacity
channels while completing that sender successfully, returns `std::nullopt` when
the channel is open and empty, and returns `ErrorKind::cancelled` only when the
channel is closed and no buffered value remains.

Unbounded channels are allowed only for **log / metric publication**, where dropping is
worse than buffering. They live in `oran-log` and are clearly named
`UnboundedLogChannel`.

## Strands & Serialization

Use a `strand<any_io_executor>` to make a coroutine "single-threaded among multiple
threads":

```cpp
auto strand = runtime.make_strand();
asio::co_spawn(strand, agent_loop.run(prompt), asio::detached);
```

Conventions:

- **One strand per agent** for ReAct iterations. Different agents truly run in parallel.
- **One strand per session DB writer** — SQLite WAL allows concurrent readers but only
  one writer; the strand enforces this without an explicit mutex.
- **Hooks** within an agent's iteration run on the agent's strand, so the agent never
  observes a hook firing mid-iteration except at the documented checkpoints.

## Timer / Sleep

`oran::async::sleep_for(executor, duration)` returns
`Awaitable<core::Result<void>>`; cancellation resolves to `ErrorKind::cancelled`.
Never use `std::this_thread::sleep_for` — it blocks the executor thread.

## Detached Tasks

Detached tasks are rare and require justification. A subsystem that needs to
spawn multiple children, bound their count, collect failures, or release
borrowed state on shutdown uses `TaskGroup`. Raw detached spawn is reserved for
cases whose full lifetime is owned by a surrounding async operation or the
process runtime itself. Pattern when it is genuinely needed:

```cpp
asio::co_spawn(
    runtime.executor(),
    [&]() -> Awaitable<void> {
      try {
        co_await long_running_work();
      } catch (const std::exception& e) {
        oran::log::error("detached task failed: {}", e.what());
      }
    },
    asio::detached);
```

The exception-to-log pattern is required because nothing else will catch them.
Detached tasks **must** be cancel-aware so shutdown completes promptly.

## Sender/Receiver Compatibility

We do **not** adopt `stdexec`. Pros of `stdexec` (composability, type-erased pipelines)
were not worth the compile-time and toolchain coupling cost in v1 and are not adopted
in v2. If someone proposes a sender-based subsystem, the answer is "wrap it behind an
`Awaitable<T>` boundary so the rest of the codebase doesn't see it." Sender/receiver
stays a private implementation detail of, at most, one library.

## Coroutine Allocation Awareness

GCC 16.1 supports the C++26 paper P2025 ("Guaranteed copy elision for return values
of coroutines") and improved HALO (heap-allocation-elision optimization). To keep
allocations rare:

- Mark inner-loop coroutines `[[nodiscard]]`.
- Pass owned data by value at the suspend boundary so escape analysis can elide.
- Avoid `std::function` on the coroutine path; prefer typed concept-bounded callbacks.

`bench/async/` currently compares a direct coroutine post loop against a bounded
`Channel<T>` handoff. Add callback-vs-awaitable scenarios when a callback-shaped
adapter exists to compare against.

## Why Not std::async / std::thread / std::jthread?

| Concern        | std::thread / std::jthread          | asio + coroutines              |
| -------------- | ----------------------------------- | ------------------------------ |
| Compile cost   | Low                                 | Modest (asio is reasonable)    |
| Cancellation   | `jthread::stop_token` only at thread boundary | First-class, fine-grained |
| Composition    | Manual                              | `co_await` everywhere          |
| Backpressure   | DIY                                 | `Channel<T>`                   |
| Strand semantics | DIY                                | `asio::strand`                 |
| Mock for tests | Hard                                | `runtime` is a single seam     |
| Resource use   | One thread per long-running task    | Many coroutines on N threads   |

We pay a bit of compile cost (mitigated by PCH + module boundaries) for a cohesive
single-language async model that is genuinely scalable. The legacy `std::thread`-free
rule in `orangutan/` was the right call; we keep it.

## Testing Async Code

- Use `asio::io_context` directly in unit tests for coroutine primitives; create
  `Runtime` only when the test is specifically about runtime ownership.
- `tests/test-helpers/run_async.hpp` owns the shared `run_async(...)` helper with a
  hard timeout. `tests/async` and `tests/io` both use it.
- Time-dependent production code uses real `steady_timer` in slice 1. A mock clock
  can land when the first scheduler/automation feature needs deterministic virtual
  time.

## Pitfalls Flagged In Review

- Calling synchronous SQLite from inside a coroutine without `asio::post(cpu_executor)`
  — blocks the io thread. Use the storage library's async wrappers.
- Returning `Awaitable<T>` from a function that captures references to locals — easy
  to dangle.
- Spawning a coroutine on the wrong executor (e.g., spawning a tool on the agent's
  strand while the strand is also doing the loop — deadlock potential if the tool
  awaits a future on the same strand).
- `co_await` inside a destructor — coroutine state will leak. Make cleanup explicit.
