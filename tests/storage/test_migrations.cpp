// tests/storage/test_migrations.cpp — storage migration runner coverage.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <oran/storage.hpp>

namespace core = orangutan::core;
namespace storage = orangutan::storage;

namespace {

class TempDb {
public:
  explicit TempDb(std::string name)
      : path_(std::filesystem::temp_directory_path() /
              (std::move(name) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
               ".db")) {}

  ~TempDb() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    std::filesystem::remove(path_.string() + "-wal", ec);
    std::filesystem::remove(path_.string() + "-shm", ec);
  }

  [[nodiscard]] std::string string() const {
    return path_.string();
  }

private:
  std::filesystem::path path_;
};

storage::Connection open_memory() {
  auto connection = storage::Connection::open(storage::ConnectionOptions{.path = ":memory:", .enable_wal = false});
  REQUIRE(connection.has_value());
  return std::move(*connection);
}

std::vector<storage::Migration> item_migrations() {
  return {
      storage::Migration{.version = 1, .name = "create-items", .sql = "CREATE TABLE items(id INTEGER PRIMARY KEY)"},
      storage::Migration{.version = 2, .name = "add-name", .sql = "ALTER TABLE items ADD COLUMN name TEXT"},
  };
}

}  // namespace

TEST_CASE("run_migrations applies pending migrations and records versions", "[unit][storage][migrations]") {
  auto connection = open_memory();
  auto migrations = item_migrations();

  auto report = storage::run_migrations(connection, migrations);

  REQUIRE(report.has_value());
  REQUIRE(report->previous_version == 0);
  REQUIRE(report->current_version == 2);
  REQUIRE(report->applied_versions == std::vector<std::int64_t>{1, 2});

  REQUIRE(connection.execute("INSERT INTO items(id, name) VALUES (1, 'alpha')").has_value());
  auto versions = connection.query("SELECT version, name FROM schema_versions ORDER BY version");
  REQUIRE(versions.has_value());
  REQUIRE(versions->rows.size() == 2);
  REQUIRE(versions->rows[0].values[0] == "1");
  REQUIRE(versions->rows[0].values[1] == "create-items");
  REQUIRE(versions->rows[1].values[0] == "2");
  REQUIRE(versions->rows[1].values[1] == "add-name");
}

TEST_CASE("run_migrations reruns as an idempotent no-op", "[unit][storage][migrations]") {
  auto connection = open_memory();
  auto migrations = item_migrations();
  REQUIRE(storage::run_migrations(connection, migrations).has_value());

  auto report = storage::run_migrations(connection, migrations);

  REQUIRE(report.has_value());
  REQUIRE(report->previous_version == 2);
  REQUIRE(report->current_version == 2);
  REQUIRE(report->applied_versions.empty());
}

TEST_CASE("run_migrations rejects invalid migration lists before applying", "[unit][storage][migrations]") {
  auto connection = open_memory();

  SECTION("non-positive version") {
    const std::vector<storage::Migration> migrations{
        storage::Migration{.version = 0, .name = "bad", .sql = "CREATE TABLE bad(id INTEGER)"},
    };
    auto result = storage::run_migrations(connection, migrations);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }

  SECTION("gap") {
    const std::vector<storage::Migration> migrations{
        storage::Migration{.version = 1, .name = "one", .sql = "CREATE TABLE one(id INTEGER)"},
        storage::Migration{.version = 3, .name = "three", .sql = "CREATE TABLE three(id INTEGER)"},
    };
    auto result = storage::run_migrations(connection, migrations);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }

  SECTION("duplicate") {
    const std::vector<storage::Migration> migrations{
        storage::Migration{.version = 1, .name = "one", .sql = "CREATE TABLE one(id INTEGER)"},
        storage::Migration{.version = 1, .name = "again", .sql = "CREATE TABLE again(id INTEGER)"},
    };
    auto result = storage::run_migrations(connection, migrations);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }

  SECTION("empty name") {
    const std::vector<storage::Migration> migrations{
        storage::Migration{.version = 1, .name = "", .sql = "CREATE TABLE one(id INTEGER)"},
    };
    auto result = storage::run_migrations(connection, migrations);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }

  SECTION("empty sql") {
    const std::vector<storage::Migration> migrations{
        storage::Migration{.version = 1, .name = "empty", .sql = ""},
    };
    auto result = storage::run_migrations(connection, migrations);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }
}

TEST_CASE("run_migrations rolls back a failed migration", "[unit][storage][migrations]") {
  auto connection = open_memory();
  const std::vector<storage::Migration> migrations{
      storage::Migration{.version = 1, .name = "create-stable", .sql = "CREATE TABLE stable(id INTEGER)"},
      storage::Migration{
          .version = 2,
          .name = "bad-second",
          .sql = "CREATE TABLE transient(id INTEGER); INSERT INTO missing_table(id) VALUES (1)",
      },
  };

  auto result = storage::run_migrations(connection, migrations);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::storage);

  auto versions = connection.query("SELECT version FROM schema_versions ORDER BY version");
  REQUIRE(versions.has_value());
  REQUIRE(versions->rows.size() == 1);
  REQUIRE(versions->rows[0].values[0] == "1");

  auto transient = connection.query("SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'transient'");
  REQUIRE(transient.has_value());
  REQUIRE(transient->rows.empty());
}

TEST_CASE("run_migrations rejects databases newer than the migration set", "[unit][storage][migrations]") {
  auto connection = open_memory();
  auto migrations = item_migrations();
  REQUIRE(storage::run_migrations(connection, migrations).has_value());

  const std::vector<storage::Migration> older_binary{
      storage::Migration{.version = 1, .name = "create-items", .sql = "CREATE TABLE items(id INTEGER PRIMARY KEY)"},
  };
  auto result = storage::run_migrations(connection, older_binary);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::conflict);
}

TEST_CASE("run_migrations detects non-contiguous recorded versions", "[unit][storage][migrations]") {
  auto connection = open_memory();
  REQUIRE(connection.execute("CREATE TABLE schema_versions(version INTEGER PRIMARY KEY, name TEXT, applied_at TEXT)")
              .has_value());
  REQUIRE(connection.execute("INSERT INTO schema_versions(version, name, applied_at) VALUES (1, 'one', 'now')")
              .has_value());
  REQUIRE(connection.execute("INSERT INTO schema_versions(version, name, applied_at) VALUES (3, 'three', 'now')")
              .has_value());

  const std::vector<storage::Migration> migrations{
      storage::Migration{.version = 1, .name = "one", .sql = "CREATE TABLE one(id INTEGER)"},
      storage::Migration{.version = 2, .name = "two", .sql = "CREATE TABLE two(id INTEGER)"},
      storage::Migration{.version = 3, .name = "three", .sql = "CREATE TABLE three(id INTEGER)"},
  };
  auto result = storage::run_migrations(connection, migrations);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::conflict);
}

TEST_CASE("run_migrations fails expected-only on read-only databases that need schema setup",
          "[unit][storage][migrations]") {
  TempDb db{"oran-storage-migration-readonly"};
  {
    auto writable = storage::Connection::open(storage::ConnectionOptions{.path = db.string(), .enable_wal = false});
    REQUIRE(writable.has_value());
  }

  auto readonly = storage::Connection::open(storage::ConnectionOptions{
      .path = db.string(),
      .mode = storage::OpenMode::read_only,
      .enable_wal = false,
      .enforce_foreign_keys = false,
  });
  REQUIRE(readonly.has_value());

  const std::vector<storage::Migration> migrations{
      storage::Migration{.version = 1, .name = "create-items", .sql = "CREATE TABLE items(id INTEGER PRIMARY KEY)"},
  };
  auto result = storage::run_migrations(*readonly, migrations);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::storage);
}
