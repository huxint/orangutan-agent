// src/oran-storage/pool.cpp — async writer/reader connection pool implementation.

#include <oran/storage/pool.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <asio/any_io_executor.hpp>
#include <asio/associated_cancellation_slot.hpp>
#include <asio/async_result.hpp>
#include <asio/cancellation_type.hpp>
#include <asio/post.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/error.hpp>

namespace orangutan::storage {

namespace {

[[nodiscard]] core::Error pool_closed_error() {
  return core::Error{core::ErrorKind::conflict, "pool is not open"};
}

}  // namespace

struct Pool::State : std::enable_shared_from_this<Pool::State> {
  explicit State(asio::any_io_executor exec) : executor{std::move(exec)} {}

  using WriterCompletion = std::move_only_function<void(core::Result<WriterLease>)>;
  using ReaderCompletion = std::move_only_function<void(core::Result<ReaderLease>)>;
  using DeferredAction = std::move_only_function<void()>;

  struct WriterWaiter {
    std::uint64_t id{};
    WriterCompletion complete;
  };
  struct ReaderWaiter {
    std::uint64_t id{};
    ReaderCompletion complete;
  };

  asio::any_io_executor executor;
  mutable std::mutex mutex;

  Connection writer;
  std::vector<Connection> readers;

  bool writer_busy{false};
  std::deque<std::size_t> free_readers;
  std::deque<WriterWaiter> writer_waiters;
  std::deque<ReaderWaiter> reader_waiters;
  std::atomic_uint64_t next_id{1};

  [[nodiscard]] std::uint64_t allocate_id() noexcept {
    return next_id.fetch_add(1, std::memory_order_relaxed);
  }

  [[nodiscard]] WriterLease make_writer_lease();
  [[nodiscard]] ReaderLease make_reader_lease(std::size_t slot);

  void release_writer() {
    WriterCompletion completion;
    {
      const std::scoped_lock lock{mutex};
      if (!writer_waiters.empty()) {
        auto waiter = std::move(writer_waiters.front());
        writer_waiters.pop_front();
        completion = std::move(waiter.complete);
      } else {
        writer_busy = false;
      }
    }
    if (completion) {
      auto lease = make_writer_lease();
      asio::post(executor, [completion = std::move(completion), lease = std::move(lease)]() mutable {
        completion(core::Result<WriterLease>{std::move(lease)});
      });
    }
  }

  void release_reader(std::size_t slot) {
    ReaderCompletion completion;
    {
      const std::scoped_lock lock{mutex};
      if (!reader_waiters.empty()) {
        auto waiter = std::move(reader_waiters.front());
        reader_waiters.pop_front();
        completion = std::move(waiter.complete);
      } else {
        free_readers.push_back(slot);
        return;
      }
    }
    if (completion) {
      auto lease = make_reader_lease(slot);
      asio::post(executor, [completion = std::move(completion), lease = std::move(lease)]() mutable {
        completion(core::Result<ReaderLease>{std::move(lease)});
      });
    }
  }

  void cancel_writer_waiter(std::uint64_t id) {
    WriterCompletion completion;
    {
      const std::scoped_lock lock{mutex};
      for (auto it = writer_waiters.begin(); it != writer_waiters.end(); ++it) {
        if (it->id == id) {
          completion = std::move(it->complete);
          writer_waiters.erase(it);
          break;
        }
      }
    }
    if (completion) {
      asio::post(executor, [completion = std::move(completion)]() mutable {
        completion(std::unexpected(core::Error::cancelled()));
      });
    }
  }

  void cancel_reader_waiter(std::uint64_t id) {
    ReaderCompletion completion;
    {
      const std::scoped_lock lock{mutex};
      for (auto it = reader_waiters.begin(); it != reader_waiters.end(); ++it) {
        if (it->id == id) {
          completion = std::move(it->complete);
          reader_waiters.erase(it);
          break;
        }
      }
    }
    if (completion) {
      asio::post(executor, [completion = std::move(completion)]() mutable {
        completion(std::unexpected(core::Error::cancelled()));
      });
    }
  }

  template <typename Handler>
  void async_acquire_writer(Handler&& handler) {
    auto cancel_slot = asio::get_associated_cancellation_slot(handler);
    const auto id = allocate_id();

    DeferredAction action;
    {
      const std::scoped_lock lock{mutex};
      if (!writer_busy) {
        writer_busy = true;
        auto lease = make_writer_lease();
        action = [handler = std::forward<Handler>(handler), lease = std::move(lease)]() mutable {
          std::move(handler)(core::Result<WriterLease>{std::move(lease)});
        };
      } else {
        writer_waiters.push_back(WriterWaiter{
            .id = id,
            .complete = WriterCompletion{std::forward<Handler>(handler)},
        });
        // Install the cancel handler after the waiter is in the queue, under
        // the same mutex. Installing earlier would leave a window where a
        // cancellation could fire, scan an empty queue, and be silently
        // dropped while the waiter is later pushed and never cancelled.
        if (cancel_slot.is_connected()) {
          const std::weak_ptr<State> weak = weak_from_this();
          cancel_slot.assign([weak, id](asio::cancellation_type type) {
            if (type == asio::cancellation_type::none) {
              return;
            }
            if (auto state = weak.lock()) {
              state->cancel_writer_waiter(id);
            }
          });
        }
      }
    }
    if (action) {
      asio::post(executor, std::move(action));
    }
  }

  template <typename Handler>
  void async_acquire_reader(Handler&& handler) {
    auto cancel_slot = asio::get_associated_cancellation_slot(handler);
    const auto id = allocate_id();

    DeferredAction action;
    {
      const std::scoped_lock lock{mutex};
      if (!free_readers.empty()) {
        const auto slot = free_readers.front();
        free_readers.pop_front();
        auto lease = make_reader_lease(slot);
        action = [handler = std::forward<Handler>(handler), lease = std::move(lease)]() mutable {
          std::move(handler)(core::Result<ReaderLease>{std::move(lease)});
        };
      } else {
        reader_waiters.push_back(ReaderWaiter{
            .id = id,
            .complete = ReaderCompletion{std::forward<Handler>(handler)},
        });
        if (cancel_slot.is_connected()) {
          const std::weak_ptr<State> weak = weak_from_this();
          cancel_slot.assign([weak, id](asio::cancellation_type type) {
            if (type == asio::cancellation_type::none) {
              return;
            }
            if (auto state = weak.lock()) {
              state->cancel_reader_waiter(id);
            }
          });
        }
      }
    }
    if (action) {
      asio::post(executor, std::move(action));
    }
  }
};

struct WriterLease::State {
  std::shared_ptr<Pool::State> pool;
  bool released{false};
};

struct ReaderLease::State {
  std::shared_ptr<Pool::State> pool;
  std::size_t slot{};
  bool released{false};
};

WriterLease Pool::State::make_writer_lease() {
  auto lease_state = std::make_shared<WriterLease::State>();
  lease_state->pool = shared_from_this();
  return WriterLease{std::move(lease_state)};
}

ReaderLease Pool::State::make_reader_lease(std::size_t slot) {
  auto lease_state = std::make_shared<ReaderLease::State>();
  lease_state->pool = shared_from_this();
  lease_state->slot = slot;
  return ReaderLease{std::move(lease_state), slot};
}

WriterLease::WriterLease(std::shared_ptr<State> state) noexcept : state_{std::move(state)} {}

WriterLease::~WriterLease() {
  release();
}

WriterLease::WriterLease(WriterLease&& other) noexcept : state_{std::move(other.state_)} {}

WriterLease& WriterLease::operator=(WriterLease&& other) noexcept {
  if (this != &other) {
    release();
    state_ = std::move(other.state_);
  }
  return *this;
}

bool WriterLease::valid() const noexcept {
  return state_ && !state_->released && state_->pool != nullptr;
}

Connection& WriterLease::connection() noexcept {
  return state_->pool->writer;
}

const Connection& WriterLease::connection() const noexcept {
  return state_->pool->writer;
}

void WriterLease::release() noexcept {
  if (!state_ || state_->released || state_->pool == nullptr) {
    state_.reset();
    return;
  }
  auto pool = std::move(state_->pool);
  state_->released = true;
  state_.reset();
  pool->release_writer();
}

ReaderLease::ReaderLease(std::shared_ptr<State> state, std::size_t slot) noexcept
    : state_{std::move(state)}, slot_{slot} {}

ReaderLease::~ReaderLease() {
  release();
}

ReaderLease::ReaderLease(ReaderLease&& other) noexcept : state_{std::move(other.state_)}, slot_{other.slot_} {
  other.slot_ = 0;
}

ReaderLease& ReaderLease::operator=(ReaderLease&& other) noexcept {
  if (this != &other) {
    release();
    state_ = std::move(other.state_);
    slot_ = other.slot_;
    other.slot_ = 0;
  }
  return *this;
}

bool ReaderLease::valid() const noexcept {
  return state_ && !state_->released && state_->pool != nullptr;
}

std::size_t ReaderLease::slot() const noexcept {
  return slot_;
}

Connection& ReaderLease::connection() noexcept {
  return state_->pool->readers[state_->slot];
}

const Connection& ReaderLease::connection() const noexcept {
  return state_->pool->readers[state_->slot];
}

void ReaderLease::release() noexcept {
  if (!state_ || state_->released || state_->pool == nullptr) {
    state_.reset();
    return;
  }
  auto pool = std::move(state_->pool);
  auto slot = state_->slot;
  state_->released = true;
  state_.reset();
  pool->release_reader(slot);
}

Pool::Pool() noexcept = default;

Pool::~Pool() = default;

Pool::Pool(Pool&&) noexcept = default;

Pool& Pool::operator=(Pool&&) noexcept = default;

core::Result<Pool> Pool::open(asio::any_io_executor executor, PoolOptions options) {
  if (options.path.empty()) {
    return std::unexpected(core::Error::invalid_argument("pool path must not be empty"));
  }
  if (options.reader_count == 0) {
    return std::unexpected(core::Error::invalid_argument("pool reader_count must be greater than zero"));
  }

  auto state = std::make_shared<State>(std::move(executor));

  auto writer = Connection::open(ConnectionOptions{
      .path = options.path,
      .mode = OpenMode::read_write_create,
      .busy_timeout_ms = options.busy_timeout_ms,
      .enable_wal = options.enable_wal,
      .enforce_foreign_keys = options.enforce_foreign_keys,
  });
  if (!writer) {
    return std::unexpected(std::move(writer.error()).with("pool_role", "writer"));
  }
  state->writer = std::move(*writer);

  state->readers.reserve(options.reader_count);
  for (std::size_t i = 0; i < options.reader_count; ++i) {
    auto reader = Connection::open(ConnectionOptions{
        .path = options.path,
        .mode = OpenMode::read_only,
        .busy_timeout_ms = options.busy_timeout_ms,
        .enable_wal = false,
        .enforce_foreign_keys = options.enforce_foreign_keys,
    });
    if (!reader) {
      return std::unexpected(
          std::move(reader.error()).with("pool_role", "reader").with("pool_slot", std::to_string(i)));
    }
    state->readers.push_back(std::move(*reader));
    state->free_readers.push_back(i);
  }

  Pool pool;
  pool.state_ = std::move(state);
  return pool;
}

bool Pool::valid() const noexcept {
  return state_ != nullptr;
}

std::size_t Pool::reader_count() const noexcept {
  return state_ ? state_->readers.size() : 0;
}

std::size_t Pool::readers_available() const noexcept {
  if (!state_) {
    return 0;
  }
  const std::scoped_lock lock{state_->mutex};
  return state_->free_readers.size();
}

bool Pool::writer_busy() const noexcept {
  if (!state_) {
    return false;
  }
  const std::scoped_lock lock{state_->mutex};
  return state_->writer_busy;
}

async::Awaitable<core::Result<WriterLease>> Pool::acquire_writer() {
  if (!state_) {
    co_return std::unexpected(pool_closed_error());
  }
  auto cancellation = co_await asio::this_coro::cancellation_state;
  if (cancellation.cancelled() != asio::cancellation_type::none) {
    co_return std::unexpected(core::Error::cancelled());
  }

  auto state = state_;
  co_return co_await asio::async_initiate<decltype(asio::use_awaitable), void(core::Result<WriterLease>)>(
      [state]<typename Handler>(Handler&& handler) mutable {
        state->async_acquire_writer(std::forward<Handler>(handler));
      },
      asio::use_awaitable);
}

async::Awaitable<core::Result<ReaderLease>> Pool::acquire_reader() {
  if (!state_) {
    co_return std::unexpected(pool_closed_error());
  }
  auto cancellation = co_await asio::this_coro::cancellation_state;
  if (cancellation.cancelled() != asio::cancellation_type::none) {
    co_return std::unexpected(core::Error::cancelled());
  }

  auto state = state_;
  co_return co_await asio::async_initiate<decltype(asio::use_awaitable), void(core::Result<ReaderLease>)>(
      [state]<typename Handler>(Handler&& handler) mutable {
        state->async_acquire_reader(std::forward<Handler>(handler));
      },
      asio::use_awaitable);
}

}  // namespace orangutan::storage
