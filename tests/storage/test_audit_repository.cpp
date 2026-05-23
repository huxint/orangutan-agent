// tests/storage/test_audit_repository.cpp — audit domain repository coverage.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/storage.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace storage = orangutan::storage;
namespace test = orangutan::tests;

namespace {

// `REQUIRE((co_await fn()).field)` evaluates `fn()` twice — Catch2's
// macro expansion captures the expression for the failure message and
// the assertion handler separately. The fix is to bind the awaitable
// result to a local first and then assert against it; every test below
// uses that two-statement shape.

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

  TempDb(const TempDb&) = delete;
  TempDb& operator=(const TempDb&) = delete;

  [[nodiscard]] std::string string() const {
    return path_.string();
  }

private:
  std::filesystem::path path_;
};

class TempDir {
public:
  explicit TempDir(std::string name)
      : path_(std::filesystem::temp_directory_path() /
              (std::move(name) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
    std::error_code ec;
    std::filesystem::create_directories(path_, ec);
    REQUIRE_FALSE(ec);
  }

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

  [[nodiscard]] std::string string() const {
    return path_.string();
  }

private:
  std::filesystem::path path_;
};

storage::Pool open_pool(asio::io_context& io, TempDb& db) {
  auto pool =
      storage::Pool::open(io.get_executor(),
                          storage::PoolOptions{.path = db.string(), .reader_count = 2, .statement_cache_capacity = 8});
  REQUIRE(pool.has_value());
  return std::move(*pool);
}

void write_file(const std::filesystem::path& path, std::string_view contents) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  REQUIRE_FALSE(ec);

  std::ofstream output{path, std::ios::binary};
  REQUIRE(output.is_open());
  output << contents;
  REQUIRE(output.good());
}

storage::AppendAuditEventRequest make_request(std::string scope_key, std::string tool_name, std::string outcome) {
  return storage::AppendAuditEventRequest{
      .scope_key = std::move(scope_key),
      .agent_key = "coder",
      .tool_name = std::move(tool_name),
      .identity = "operator-1",
      .verdict = "allow",
      .outcome = std::move(outcome),
      .reason = "rule #1 (allow: file.*)",
  };
}

}  // namespace

TEST_CASE("AuditRepository::migrate applies the audit schema once", "[unit][storage][audit_repository]") {
  TempDb db{"oran-audit-repo-migrate"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::AuditRepository repo{pool};

    auto first = co_await repo.migrate();
    REQUIRE(first.has_value());
    REQUIRE(first->previous_version == 0);
    REQUIRE(first->current_version == 2);
    REQUIRE(first->applied_versions == std::vector<std::int64_t>{1, 2});

    auto second = co_await repo.migrate();
    REQUIRE(second.has_value());
    REQUIRE(second->previous_version == 2);
    REQUIRE(second->current_version == 2);
    REQUIRE(second->applied_versions.empty());
  });
}

TEST_CASE("AuditRepository::migrate accepts an explicit migration directory", "[unit][storage][audit_repository]") {
  TempDb db{"oran-audit-repo-migrate-dir"};
  TempDir migrations{"oran-audit-repo-migrations"};
  write_file(migrations.path() / "0001-custom-marker.sql", "CREATE TABLE custom_audit_marker(id INTEGER PRIMARY KEY)");

  test::run_async([&db, &migrations](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::AuditRepository repo{
        pool,
        storage::AuditRepositoryOptions{.migrations_directory = migrations.string()},
    };

    auto report = co_await repo.migrate();
    REQUIRE(report.has_value());
    REQUIRE(report->current_version == 1);
    REQUIRE(report->applied_versions == std::vector<std::int64_t>{1});

    auto reader = co_await pool.acquire_reader();
    REQUIRE(reader.has_value());
    auto marker = reader->connection().query(
        "SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'custom_audit_marker'");
    REQUIRE(marker.has_value());
    REQUIRE(marker->rows.size() == 1);
  });
}

TEST_CASE("AuditRepository append_event round-trips a typical decision row", "[unit][storage][audit_repository]") {
  TempDb db{"oran-audit-repo-roundtrip"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::AuditRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    auto request = make_request("scope-A", "file.read", "allow");
    request.input_hash_hex = std::string(64, 'a');
    request.metadata_json = R"json({"source":"test"})json";
    auto appended = co_await repo.append_event(request);
    REQUIRE(appended.has_value());
    REQUIRE(appended->id > 0);
    REQUIRE(appended->scope_key == "scope-A");
    REQUIRE(appended->tool_name == "file.read");
    REQUIRE(appended->outcome == "allow");
    REQUIRE(appended->reason == "rule #1 (allow: file.*)");
    REQUIRE(appended->input_hash_hex.has_value());
    REQUIRE(*appended->input_hash_hex == std::string(64, 'a'));
    REQUIRE(appended->metadata_json == R"json({"source":"test"})json");
    REQUIRE_FALSE(appended->created_at.empty());

    auto listed = co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A"});
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 1);
    REQUIRE((*listed)[0].id == appended->id);
    REQUIRE((*listed)[0].tool_name == "file.read");

    auto count = co_await repo.count_events("scope-A");
    REQUIRE(count.has_value());
    REQUIRE(*count == 1);
  });
}

TEST_CASE("AuditRepository stores a null input_hash when the caller omits it", "[unit][storage][audit_repository]") {
  TempDb db{"oran-audit-repo-null-hash"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::AuditRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    auto appended = co_await repo.append_event(make_request("scope-A", "file.read", "deny"));
    REQUIRE(appended.has_value());
    REQUIRE_FALSE(appended->input_hash_hex.has_value());

    auto listed = co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A"});
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 1);
    REQUIRE_FALSE((*listed)[0].input_hash_hex.has_value());
  });
}

TEST_CASE("AuditRepository updates metadata for the matching audit row", "[unit][storage][audit_repository]") {
  TempDb db{"oran-audit-repo-update-metadata"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::AuditRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    auto request = make_request("scope-A", "file.read", "allow");
    request.input_hash_hex = std::string(64, 'b');
    request.metadata_json = R"json({"dispatch":{"sequence":7}})json";
    auto appended = co_await repo.append_event(request);
    REQUIRE(appended.has_value());

    auto updated = co_await repo.update_event_metadata(storage::UpdateAuditEventMetadataRequest{
        .scope_key = "scope-A",
        .agent_key = "coder",
        .tool_name = "file.read",
        .identity = "operator-1",
        .input_hash_hex = std::string(64, 'b'),
        .previous_metadata_json = R"json({"dispatch":{"sequence":7}})json",
        .metadata_json = R"json({"dispatch":{"sequence":7},"usage":{"bytes_read":4096}})json",
    });
    REQUIRE(updated.has_value());
    REQUIRE(updated->id == appended->id);
    REQUIRE(updated->metadata_json == R"json({"dispatch":{"sequence":7},"usage":{"bytes_read":4096}})json");

    auto listed = co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A"});
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 1);
    REQUIRE((*listed)[0].metadata_json == R"json({"dispatch":{"sequence":7},"usage":{"bytes_read":4096}})json");
  });
}

TEST_CASE("AuditRepository list_events orders newest first and applies filters", "[unit][storage][audit_repository]") {
  TempDb db{"oran-audit-repo-filters"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::AuditRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    auto a1 = co_await repo.append_event(make_request("scope-A", "file.read", "allow"));
    REQUIRE(a1.has_value());
    auto a2 = co_await repo.append_event(make_request("scope-A", "file.write", "deny"));
    REQUIRE(a2.has_value());
    auto a3 = co_await repo.append_event(make_request("scope-A", "shell.exec", "approved"));
    REQUIRE(a3.has_value());
    auto a4 = co_await repo.append_event(make_request("scope-B", "file.read", "allow"));
    REQUIRE(a4.has_value());

    auto all = co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A"});
    REQUIRE(all.has_value());
    REQUIRE(all->size() == 3);
    REQUIRE((*all)[0].tool_name == "shell.exec");
    REQUIRE((*all)[1].tool_name == "file.write");
    REQUIRE((*all)[2].tool_name == "file.read");

    auto only_deny =
        co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A", .outcome = "deny"});
    REQUIRE(only_deny.has_value());
    REQUIRE(only_deny->size() == 1);
    REQUIRE((*only_deny)[0].tool_name == "file.write");

    auto only_file_read =
        co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A", .tool_name = "file.read"});
    REQUIRE(only_file_read.has_value());
    REQUIRE(only_file_read->size() == 1);

    auto limited = co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A", .limit = 1});
    REQUIRE(limited.has_value());
    REQUIRE(limited->size() == 1);
    REQUIRE((*limited)[0].tool_name == "shell.exec");

    auto count_a = co_await repo.count_events("scope-A");
    REQUIRE(count_a.has_value());
    REQUIRE(*count_a == 3);
    auto count_b = co_await repo.count_events("scope-B");
    REQUIRE(count_b.has_value());
    REQUIRE(*count_b == 1);
  });
}

TEST_CASE("AuditRepository validates required fields", "[unit][storage][audit_repository]") {
  TempDb db{"oran-audit-repo-validate"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::AuditRepository repo{pool};

    auto missing_scope = co_await repo.append_event(storage::AppendAuditEventRequest{
        .scope_key = "",
        .agent_key = "coder",
        .tool_name = "file.read",
        .identity = "op",
        .verdict = "allow",
        .outcome = "allow",
        .reason = "rule",
    });
    REQUIRE_FALSE(missing_scope.has_value());
    REQUIRE(missing_scope.error().kind() == core::ErrorKind::invalid_argument);

    auto missing_metadata = co_await repo.append_event(storage::AppendAuditEventRequest{
        .scope_key = "scope-A",
        .agent_key = "coder",
        .tool_name = "file.read",
        .identity = "op",
        .verdict = "allow",
        .outcome = "allow",
        .reason = "rule",
        .metadata_json = "",
    });
    REQUIRE_FALSE(missing_metadata.has_value());
    REQUIRE(missing_metadata.error().kind() == core::ErrorKind::invalid_argument);

    auto list_no_scope = co_await repo.list_events(storage::ListAuditEventsOptions{});
    REQUIRE_FALSE(list_no_scope.has_value());
    REQUIRE(list_no_scope.error().kind() == core::ErrorKind::invalid_argument);

    auto list_zero_limit =
        co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A", .limit = 0});
    REQUIRE_FALSE(list_zero_limit.has_value());
    REQUIRE(list_zero_limit.error().kind() == core::ErrorKind::invalid_argument);

    auto count_no_scope = co_await repo.count_events("");
    REQUIRE_FALSE(count_no_scope.has_value());
    REQUIRE(count_no_scope.error().kind() == core::ErrorKind::invalid_argument);

    auto update_missing_metadata = co_await repo.update_event_metadata(storage::UpdateAuditEventMetadataRequest{
        .scope_key = "scope-A",
        .agent_key = "coder",
        .tool_name = "file.read",
        .identity = "op",
        .previous_metadata_json = "",
    });
    REQUIRE_FALSE(update_missing_metadata.has_value());
    REQUIRE(update_missing_metadata.error().kind() == core::ErrorKind::invalid_argument);
  });
}

TEST_CASE("AuditRepository surfaces a storage error for rows with null required fields",
          "[unit][storage][audit_repository]") {
  TempDb db{"oran-audit-repo-bad-row"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::AuditRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    {
      auto writer = co_await pool.acquire_writer();
      REQUIRE(writer.has_value());
      // Drop the NOT NULL invariant on a required field, inject a row
      // that violates the application-level invariant, then restore the
      // shape. We exercise the read-side defensive parsing, not the
      // schema's own constraints.
      auto rebuild = writer->connection().execute(R"sql(
BEGIN;
ALTER TABLE audit_events RENAME TO audit_events_strict;
CREATE TABLE audit_events(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  scope_key TEXT NOT NULL,
  agent_key TEXT NOT NULL,
  tool_name TEXT NOT NULL,
  identity TEXT NOT NULL,
  verdict TEXT NOT NULL,
  outcome TEXT NOT NULL,
  reason TEXT,
  input_hash_hex TEXT,
  metadata_json TEXT NOT NULL DEFAULT '{}',
  created_at TEXT NOT NULL
);
COMMIT;
)sql");
      REQUIRE(rebuild.has_value());
      auto inserted = writer->connection().execute(
          R"sql(
INSERT INTO audit_events(scope_key, agent_key, tool_name, identity, verdict, outcome, reason, metadata_json, created_at)
VALUES ('scope-A', 'coder', 'file.read', 'op', 'allow', 'allow', NULL, '{}', '2026-05-17T00:00:00.000Z')
)sql");
      REQUIRE(inserted.has_value());
    }

    auto listed = co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A"});
    REQUIRE_FALSE(listed.has_value());
    REQUIRE(listed.error().kind() == core::ErrorKind::storage);
  });
}

TEST_CASE("AuditRepository returns empty results for missing scopes", "[unit][storage][audit_repository]") {
  TempDb db{"oran-audit-repo-empty"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::AuditRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    auto listed = co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-empty"});
    REQUIRE(listed.has_value());
    REQUIRE(listed->empty());

    auto count = co_await repo.count_events("scope-empty");
    REQUIRE(count.has_value());
    REQUIRE(*count == 0);
  });
}
