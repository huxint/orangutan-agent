// bench/storage/scenarios/pool_acquire.cpp
//
// A-vs-B comparison: direct Connection re-use vs. async Pool acquire + release
// for an identical SELECT workload. Both paths run on the same in-memory schema
// so only the acquisition overhead differs.

#include <nanobench.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <oran/async.hpp>
#include <oran/storage.hpp>

namespace orangutan::bench {

namespace {

constexpr std::size_t kReaderCount = 4;
constexpr int kBatchQueries = 16;

std::string make_temp_path(std::string_view tag) {
  auto path = std::filesystem::temp_directory_path() /
              (std::string{"oran-bench-"} + std::string{tag} + "-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
  return path.string();
}

void seed(const std::string& path) {
  auto writer = storage::Connection::open(storage::ConnectionOptions{.path = path});
  if (!writer) {
    std::abort();
  }
  if (!writer->execute("CREATE TABLE IF NOT EXISTS rows_(id INTEGER PRIMARY KEY)")) {
    std::abort();
  }
  for (int i = 0; i < 32; ++i) {
    auto insert = writer->execute("INSERT INTO rows_(id) VALUES (NULL)");
    if (!insert) {
      std::abort();
    }
  }
}

[[gnu::noinline]] std::int64_t run_direct(storage::Connection& reader) {
  std::int64_t accum = 0;
  for (int i = 0; i < kBatchQueries; ++i) {
    auto result = reader.query("SELECT COUNT(*) FROM rows_");
    if (!result || result->rows.empty() || !result->rows.front().values.front()) {
      std::abort();
    }
    accum += std::stoll(*result->rows.front().values.front());
  }
  return accum;
}

[[gnu::noinline]] std::int64_t run_pool(asio::io_context& io, storage::Pool& pool) {
  std::int64_t accum = 0;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        for (int i = 0; i < kBatchQueries; ++i) {
          auto lease = co_await pool.acquire_reader();
          if (!lease) {
            std::abort();
          }
          auto result = lease->connection().query("SELECT COUNT(*) FROM rows_");
          if (!result || result->rows.empty() || !result->rows.front().values.front()) {
            std::abort();
          }
          accum += std::stoll(*result->rows.front().values.front());
        }
        co_return;
      },
      asio::detached);
  io.restart();
  io.run();
  return accum;
}

class TempDb {
public:
  explicit TempDb(std::string_view tag) : path_{make_temp_path(tag)} {}

  ~TempDb() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    std::filesystem::remove(path_ + "-wal", ec);
    std::filesystem::remove(path_ + "-shm", ec);
  }

  TempDb(const TempDb&) = delete;
  TempDb& operator=(const TempDb&) = delete;

  [[nodiscard]] const std::string& path() const noexcept {
    return path_;
  }

private:
  std::string path_;
};

}  // namespace

void register_pool_acquire(ankerl::nanobench::Bench& bench) {
  TempDb db{"pool"};
  seed(db.path());

  auto reader = storage::Connection::open(storage::ConnectionOptions{
      .path = db.path(),
      .mode = storage::OpenMode::read_only,
      .enable_wal = false,
  });
  if (!reader) {
    std::abort();
  }

  asio::io_context io;
  auto pool =
      storage::Pool::open(io.get_executor(), storage::PoolOptions{.path = db.path(), .reader_count = kReaderCount});
  if (!pool) {
    std::abort();
  }

  bench.run("storage.direct_connection_query", [&] {
    auto result = run_direct(*reader);
    ankerl::nanobench::doNotOptimizeAway(result);
  });
  bench.run("storage.pool_acquire_query", [&] {
    auto result = run_pool(io, *pool);
    ankerl::nanobench::doNotOptimizeAway(result);
  });
}

}  // namespace orangutan::bench
