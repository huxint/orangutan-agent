// bench/storage/scenarios/migrations.cpp
//
// A-vs-B comparison: cold migration apply vs. no-op migration check.

#include <nanobench.h>

#include <cstdint>
#include <cstdlib>
#include <format>
#include <string>
#include <vector>

#include <oran/storage.hpp>

namespace orangutan::bench {

namespace {

constexpr int kMigrationCount = 16;

std::vector<storage::Migration> make_migrations() {
  std::vector<storage::Migration> migrations;
  migrations.reserve(kMigrationCount);
  for (int i = 1; i <= kMigrationCount; ++i) {
    migrations.push_back(storage::Migration{
        .version = i,
        .name = std::format("create-table-{}", i),
        .sql = std::format("CREATE TABLE table_{}(id INTEGER PRIMARY KEY, value TEXT)", i),
    });
  }
  return migrations;
}

storage::Connection make_connection() {
  auto connection = storage::Connection::open(storage::ConnectionOptions{.path = ":memory:", .enable_wal = false});
  if (!connection) {
    std::abort();
  }
  return std::move(*connection);
}

[[gnu::noinline]] std::int64_t cold_apply(const std::vector<storage::Migration>& migrations) {
  auto connection = make_connection();
  auto report = storage::run_migrations(connection, migrations);
  if (!report) {
    std::abort();
  }
  return report->current_version + static_cast<std::int64_t>(report->applied_versions.size());
}

[[gnu::noinline]] std::int64_t noop_check(storage::Connection& connection,
                                          const std::vector<storage::Migration>& migrations) {
  auto report = storage::run_migrations(connection, migrations);
  if (!report) {
    std::abort();
  }
  return report->current_version + static_cast<std::int64_t>(report->applied_versions.size());
}

}  // namespace

void register_migrations(ankerl::nanobench::Bench& bench) {
  const auto migrations = make_migrations();
  auto migrated = make_connection();
  if (!storage::run_migrations(migrated, migrations)) {
    std::abort();
  }

  bench.run("storage.migrations_cold_apply", [&] {
    auto result = cold_apply(migrations);
    ankerl::nanobench::doNotOptimizeAway(result);
  });
  bench.run("storage.migrations_noop_check", [&] {
    auto result = noop_check(migrated, migrations);
    ankerl::nanobench::doNotOptimizeAway(result);
  });
}

}  // namespace orangutan::bench
