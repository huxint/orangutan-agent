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
#include <oran/core/turn_id.hpp>
#include <oran/storage.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace storage = orangutan::storage;
namespace test = orangutan::tests;

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
      .reason = "rule #1 (allow: File*)",
  };
}

core::TurnId turn_id_with(unsigned char seed) {
  core::TurnId id{};
  for (std::size_t i = 0; i < id.size(); ++i) {
    id[i] = static_cast<std::byte>(seed + i);
  }
  return id;
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
    REQUIRE(first->current_version == 5);
    REQUIRE(first->applied_versions == std::vector<std::int64_t>{1, 2, 3, 4, 5});

    auto second = co_await repo.migrate();
    REQUIRE(second.has_value());
    REQUIRE(second->previous_version == 5);
    REQUIRE(second->current_version == 5);
    REQUIRE(second->applied_versions.empty());

    auto reader = co_await pool.acquire_reader();
    REQUIRE(reader.has_value());
    auto view = reader->connection().query(
        "SELECT name FROM sqlite_master WHERE type = 'view' AND name = 'audit_tool_call_rollups'");
    REQUIRE(view.has_value());
    REQUIRE(view->rows.size() == 1);
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

TEST_CASE("AuditRepository reopening preserves saved audits traces and views",
          "[unit][storage][audit_repository][preservation]") {
  TempDb db{"oran-audit-repo-preservation"};
  {
    auto connection = storage::Connection::open({.path = db.string()});
    REQUIRE(connection.has_value());
    auto migrated = storage::run_migrations(*connection, storage::built_in_audit_migrations());
    REQUIRE(migrated.has_value());
    auto seeded = connection->execute(R"sql(
      INSERT INTO trace_turns VALUES (
        X'101112131415161718191A1B1C1D1E1F', NULL, X'808182838485868788898A8B8C8D8E8F',
        'coder', 'cli', 'provider-main', 'model-a', 100, 125, 'cancelled', 2,
        42, 1024, 43, 44, 5, 7, 30, 10, 0.5, 'tools', CAST('{"source":"saved"}' AS BLOB), 1
      );
      INSERT INTO audit_events (
        id, scope_key, agent_key, tool_name, identity, verdict, outcome, reason,
        input_hash_hex, metadata_json, created_at, parent_turn_id, event_kind
      ) VALUES (
        17, 'scope-A', 'coder', 'FileWrite', 'saved-operator', 'ask', 'approved', 'saved approval',
        'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
        '{"usage":{"wall_time_ms":5.5}}', '2000-01-01T00:00:00Z',
        X'101112131415161718191A1B1C1D1E1F', 'permission_decision'
      );
    )sql");
    REQUIRE(seeded.has_value());
  }

  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::AuditRepository audits{pool};
    storage::TraceRepository traces{pool};
    auto migrated = co_await audits.migrate();
    REQUIRE(migrated.has_value());
    REQUIRE(migrated->previous_version == 5);
    REQUIRE(migrated->current_version == 5);
    REQUIRE(migrated->applied_versions.empty());
    auto trace_migrated = co_await traces.migrate();
    REQUIRE(trace_migrated.has_value());
    REQUIRE(trace_migrated->applied_versions.empty());

    auto appended = co_await audits.append_event(make_request("scope-A", "FileRead", "allow"));
    REQUIRE(appended.has_value());
    REQUIRE(appended->id == 18);
    auto traced = co_await traces.append_turn({.turn_id = turn_id_with(0x20),
                                              .session_id = turn_id_with(0x80),
                                              .agent_key = "coder",
                                              .origin = "cli",
                                              .route_profile = "provider-main",
                                              .route_model = "model-a",
                                              .started_at_ns = 200,
                                              .finished_at_ns = 225,
                                              .stop_reason = "end_turn"});
    REQUIRE(traced.has_value());

    auto events = co_await audits.list_events_for_turn(turn_id_with(0x10));
    REQUIRE(events.has_value());
    REQUIRE(events->size() == 1);
    const auto& saved = events->front();
    REQUIRE(saved.id == 17);
    REQUIRE(saved.identity == "saved-operator");
    REQUIRE(saved.verdict == "ask");
    REQUIRE(saved.outcome == "approved");
    REQUIRE(saved.reason == "saved approval");
    REQUIRE(saved.input_hash_hex == std::string(64, 'a'));
    REQUIRE(saved.metadata_json == R"({"usage":{"wall_time_ms":5.5}})");
    REQUIRE(saved.created_at == "2000-01-01T00:00:00Z");
    auto turn = co_await traces.get_turn(turn_id_with(0x10));
    REQUIRE(turn.has_value());
    REQUIRE(turn->has_value());
    REQUIRE((*turn)->session_id == turn_id_with(0x80));
    REQUIRE((*turn)->started_at_ns == 100);
    REQUIRE((*turn)->finished_at_ns == 125);
    REQUIRE((*turn)->stop_reason == "cancelled");
    REQUIRE((*turn)->input_tokens == 30);
    REQUIRE((*turn)->output_tokens == 10);
    REQUIRE((*turn)->cancellation_phase == "tools");
    REQUIRE((*turn)->context_json == R"({"source":"saved"})");
    auto count = co_await traces.count_turns();
    REQUIRE(count.has_value());
    REQUIRE(*count == 2);

    auto reader = co_await pool.acquire_reader();
    REQUIRE(reader.has_value());
    auto view = reader->connection().query(
        "SELECT tool_name, decision_count, permitted_count, total_wall_time_ms FROM audit_tool_call_rollups");
    REQUIRE(view.has_value());
    REQUIRE(view->rows.size() == 1);
    REQUIRE(view->rows.front().values == std::vector<storage::ColumnValue>{"FileWrite", "1", "1", "5.5"});
  });
}

TEST_CASE("AuditRepository append_event round-trips a typical decision row", "[unit][storage][audit_repository]") {
  TempDb db{"oran-audit-repo-roundtrip"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::AuditRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    auto request = make_request("scope-A", "FileRead", "allow");
    request.input_hash_hex = std::string(64, 'a');
    request.metadata_json = R"json({"source":"test"})json";
    auto appended = co_await repo.append_event(request);
    REQUIRE(appended.has_value());
    REQUIRE(appended->id > 0);
    REQUIRE(appended->event_kind == "permission_decision");
    REQUIRE(appended->scope_key == "scope-A");
    REQUIRE(appended->tool_name == "FileRead");
    REQUIRE(appended->outcome == "allow");
    REQUIRE(appended->reason == "rule #1 (allow: File*)");
    REQUIRE(appended->input_hash_hex.has_value());
    REQUIRE(*appended->input_hash_hex == std::string(64, 'a'));
    REQUIRE_FALSE(appended->parent_turn_id.has_value());
    REQUIRE(appended->metadata_json == R"json({"source":"test"})json");
    REQUIRE_FALSE(appended->created_at.empty());

    auto listed = co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A"});
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 1);
    REQUIRE((*listed)[0].id == appended->id);
    REQUIRE((*listed)[0].event_kind == "permission_decision");
    REQUIRE((*listed)[0].tool_name == "FileRead");
    REQUIRE_FALSE((*listed)[0].parent_turn_id.has_value());

    auto count = co_await repo.count_events("scope-A");
    REQUIRE(count.has_value());
    REQUIRE(*count == 1);
  });
}

TEST_CASE("AuditRepository round-trips parent_turn_id blobs", "[unit][storage][audit_repository]") {
  TempDb db{"oran-audit-repo-parent-turn"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::AuditRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    auto request = make_request("scope-A", "FileRead", "allow");
    request.input_hash_hex = std::string(64, 'c');
    request.parent_turn_id = turn_id_with(0x10);
    auto appended = co_await repo.append_event(request);
    REQUIRE(appended.has_value());
    REQUIRE(appended->parent_turn_id.has_value());
    REQUIRE(*appended->parent_turn_id == turn_id_with(0x10));

    auto listed = co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A"});
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 1);
    REQUIRE((*listed)[0].parent_turn_id.has_value());
    REQUIRE(*(*listed)[0].parent_turn_id == turn_id_with(0x10));
  });
}

TEST_CASE("AuditRepository stores a null input_hash when the caller omits it", "[unit][storage][audit_repository]") {
  TempDb db{"oran-audit-repo-null-hash"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::AuditRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    auto appended = co_await repo.append_event(make_request("scope-A", "FileRead", "deny"));
    REQUIRE(appended.has_value());
    REQUIRE_FALSE(appended->input_hash_hex.has_value());

    auto listed = co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A"});
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 1);
    REQUIRE_FALSE((*listed)[0].input_hash_hex.has_value());
  });
}

TEST_CASE("AuditRepository metadata update is scoped by parent_turn_id", "[unit][storage][audit_repository]") {
  TempDb db{"oran-audit-repo-update-parent-turn"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::AuditRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    auto first = make_request("scope-A", "FileRead", "allow");
    first.input_hash_hex = std::string(64, 'd');
    first.parent_turn_id = turn_id_with(0x10);
    first.metadata_json = R"json({"dispatch":{"sequence":1}})json";
    auto appended_first = co_await repo.append_event(first);
    REQUIRE(appended_first.has_value());

    auto second = first;
    second.parent_turn_id = turn_id_with(0x40);
    auto appended_second = co_await repo.append_event(second);
    REQUIRE(appended_second.has_value());

    auto updated = co_await repo.update_event_metadata(storage::UpdateAuditEventMetadataRequest{
        .scope_key = "scope-A",
        .agent_key = "coder",
        .tool_name = "FileRead",
        .identity = "operator-1",
        .input_hash_hex = std::string(64, 'd'),
        .parent_turn_id = turn_id_with(0x10),
        .previous_metadata_json = R"json({"dispatch":{"sequence":1}})json",
        .metadata_json = R"json({"dispatch":{"sequence":1},"usage":{"files_touched":1}})json",
    });
    REQUIRE(updated.has_value());
    REQUIRE(updated->id == appended_first->id);

    auto listed = co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A", .limit = 10});
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 2);
    REQUIRE((*listed)[0].id == appended_second->id);
    REQUIRE((*listed)[0].metadata_json == R"json({"dispatch":{"sequence":1}})json");
    REQUIRE((*listed)[1].id == appended_first->id);
    REQUIRE((*listed)[1].metadata_json == R"json({"dispatch":{"sequence":1},"usage":{"files_touched":1}})json");
  });
}

TEST_CASE("AuditRepository updates metadata for the matching audit row", "[unit][storage][audit_repository]") {
  TempDb db{"oran-audit-repo-update-metadata"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::AuditRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    auto request = make_request("scope-A", "FileRead", "allow");
    request.input_hash_hex = std::string(64, 'b');
    request.metadata_json = R"json({"dispatch":{"sequence":7}})json";
    auto appended = co_await repo.append_event(request);
    REQUIRE(appended.has_value());

    auto updated = co_await repo.update_event_metadata(storage::UpdateAuditEventMetadataRequest{
        .scope_key = "scope-A",
        .agent_key = "coder",
        .tool_name = "FileRead",
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

    auto a1 = co_await repo.append_event(make_request("scope-A", "FileRead", "allow"));
    REQUIRE(a1.has_value());
    auto a2 = co_await repo.append_event(make_request("scope-A", "FileWrite", "deny"));
    REQUIRE(a2.has_value());
    auto a3 = co_await repo.append_event(make_request("scope-A", "ShellExec", "approved"));
    REQUIRE(a3.has_value());
    auto a4 = co_await repo.append_event(make_request("scope-B", "FileRead", "allow"));
    REQUIRE(a4.has_value());
    auto hook_publish = make_request("scope-A", "FileRead", "allow");
    hook_publish.event_kind = "hook_publish";
    hook_publish.metadata_json = R"json({"event":"tool_before","sink_id":"policy","decision_kind":"veto"})json";
    auto a5 = co_await repo.append_event(std::move(hook_publish));
    REQUIRE(a5.has_value());

    auto all = co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A"});
    REQUIRE(all.has_value());
    REQUIRE(all->size() == 4);
    REQUIRE((*all)[0].event_kind == "hook_publish");
    REQUIRE((*all)[1].tool_name == "ShellExec");
    REQUIRE((*all)[2].tool_name == "FileWrite");
    REQUIRE((*all)[3].tool_name == "FileRead");

    auto only_deny =
        co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A", .outcome = "deny"});
    REQUIRE(only_deny.has_value());
    REQUIRE(only_deny->size() == 1);
    REQUIRE((*only_deny)[0].tool_name == "FileWrite");

    auto only_file_read =
        co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A", .tool_name = "FileRead"});
    REQUIRE(only_file_read.has_value());
    REQUIRE(only_file_read->size() == 2);

    auto only_hook_publish = co_await repo.list_events(
        storage::ListAuditEventsOptions{.scope_key = "scope-A", .event_kind = "hook_publish"});
    REQUIRE(only_hook_publish.has_value());
    REQUIRE(only_hook_publish->size() == 1);
    REQUIRE((*only_hook_publish)[0].id == a5->id);
    REQUIRE((*only_hook_publish)[0].metadata_json ==
            R"json({"event":"tool_before","sink_id":"policy","decision_kind":"veto"})json");

    auto limited = co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A", .limit = 1});
    REQUIRE(limited.has_value());
    REQUIRE(limited->size() == 1);
    REQUIRE((*limited)[0].event_kind == "hook_publish");

    auto count_a = co_await repo.count_events("scope-A");
    REQUIRE(count_a.has_value());
    REQUIRE(*count_a == 4);
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
        .tool_name = "FileRead",
        .identity = "op",
        .verdict = "allow",
        .outcome = "allow",
        .reason = "rule",
    });
    REQUIRE_FALSE(missing_scope.has_value());
    REQUIRE(missing_scope.error().kind() == core::ErrorKind::invalid_argument);

    auto missing_event_kind = make_request("scope-A", "FileRead", "allow");
    missing_event_kind.event_kind = "";
    auto missing_event_kind_result = co_await repo.append_event(std::move(missing_event_kind));
    REQUIRE_FALSE(missing_event_kind_result.has_value());
    REQUIRE(missing_event_kind_result.error().kind() == core::ErrorKind::invalid_argument);

    auto missing_metadata = co_await repo.append_event(storage::AppendAuditEventRequest{
        .scope_key = "scope-A",
        .agent_key = "coder",
        .tool_name = "FileRead",
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
        .tool_name = "FileRead",
        .identity = "op",
        .previous_metadata_json = "",
    });
    REQUIRE_FALSE(update_missing_metadata.has_value());
    REQUIRE(update_missing_metadata.error().kind() == core::ErrorKind::invalid_argument);

    auto zero_parent = make_request("scope-A", "FileRead", "allow");
    zero_parent.parent_turn_id = core::TurnId{};
    auto zero_parent_result = co_await repo.append_event(std::move(zero_parent));
    REQUIRE_FALSE(zero_parent_result.has_value());
    REQUIRE(zero_parent_result.error().kind() == core::ErrorKind::invalid_argument);

    auto update_zero_parent = co_await repo.update_event_metadata(storage::UpdateAuditEventMetadataRequest{
        .scope_key = "scope-A",
        .agent_key = "coder",
        .tool_name = "FileRead",
        .identity = "op",
        .parent_turn_id = core::TurnId{},
    });
    REQUIRE_FALSE(update_zero_parent.has_value());
    REQUIRE(update_zero_parent.error().kind() == core::ErrorKind::invalid_argument);
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
  event_kind TEXT NOT NULL DEFAULT 'permission_decision',
  scope_key TEXT NOT NULL,
  agent_key TEXT NOT NULL,
  tool_name TEXT NOT NULL,
  identity TEXT NOT NULL,
  verdict TEXT NOT NULL,
  outcome TEXT NOT NULL,
  reason TEXT,
  input_hash_hex TEXT,
  parent_turn_id BLOB,
  metadata_json TEXT NOT NULL DEFAULT '{}',
  created_at TEXT NOT NULL
);
COMMIT;
)sql");
      REQUIRE(rebuild.has_value());
      auto inserted = writer->connection().execute(
          R"sql(
INSERT INTO audit_events(scope_key, agent_key, tool_name, identity, verdict, outcome, reason, metadata_json, created_at)
VALUES ('scope-A', 'coder', 'FileRead', 'op', 'allow', 'allow', NULL, '{}', '2026-05-17T00:00:00.000Z')
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

TEST_CASE("AuditRepository::list_events_for_turn preserves dispatch order and ignores scope",
          "[unit][storage][audit_repository][trace]") {
  TempDb db{"oran-audit-repo-list-for-turn"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::AuditRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    const auto turn_a = turn_id_with(0x10);
    const auto turn_b = turn_id_with(0x20);

    // Three rows belong to turn A under two scopes; one belongs to turn B; one
    // has no parent turn. The trace inspector must surface every turn-A row
    // regardless of scope and skip the unrelated rows.
    auto first = make_request("scope-A", "FileRead", "allow");
    first.parent_turn_id = turn_a;
    auto first_row = co_await repo.append_event(std::move(first));
    REQUIRE(first_row.has_value());

    auto second = make_request("scope-A", "FileWrite", "allow");
    second.event_kind = "hook_publish";
    second.parent_turn_id = turn_a;
    second.metadata_json = R"json({"event":"tool_before","sink_id":"policy","decision_kind":"proceed"})json";
    auto second_row = co_await repo.append_event(std::move(second));
    REQUIRE(second_row.has_value());

    auto third = make_request("scope-B", "FileEdit", "allow");
    third.parent_turn_id = turn_a;
    auto third_row = co_await repo.append_event(std::move(third));
    REQUIRE(third_row.has_value());

    auto unrelated_turn = make_request("scope-A", "FileRead", "deny");
    unrelated_turn.parent_turn_id = turn_b;
    auto unrelated_turn_row = co_await repo.append_event(std::move(unrelated_turn));
    REQUIRE(unrelated_turn_row.has_value());

    auto no_turn = make_request("scope-A", "FileRead", "allow");
    auto no_turn_row = co_await repo.append_event(std::move(no_turn));
    REQUIRE(no_turn_row.has_value());

    auto joined = co_await repo.list_events_for_turn(turn_a);
    REQUIRE(joined.has_value());
    REQUIRE(joined->size() == 3);
    REQUIRE((*joined)[0].id == first_row->id);
    REQUIRE((*joined)[1].id == second_row->id);
    REQUIRE((*joined)[2].id == third_row->id);
    REQUIRE((*joined)[0].event_kind == "permission_decision");
    REQUIRE((*joined)[1].event_kind == "hook_publish");
    REQUIRE((*joined)[2].event_kind == "permission_decision");
    REQUIRE((*joined)[0].tool_name == "FileRead");
    REQUIRE((*joined)[1].tool_name == "FileWrite");
    REQUIRE((*joined)[2].tool_name == "FileEdit");
    REQUIRE((*joined)[2].scope_key == "scope-B");

    auto limited = co_await repo.list_events_for_turn(turn_a, 2);
    REQUIRE(limited.has_value());
    REQUIRE(limited->size() == 2);
    REQUIRE((*limited)[0].id == first_row->id);
    REQUIRE((*limited)[1].id == second_row->id);

    auto missing = co_await repo.list_events_for_turn(turn_id_with(0x99));
    REQUIRE(missing.has_value());
    REQUIRE(missing->empty());
  });
}

TEST_CASE("AuditRepository::list_events_for_turn rejects malformed inputs",
          "[unit][storage][audit_repository][trace]") {
  TempDb db{"oran-audit-repo-list-for-turn-validate"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::AuditRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    auto zero_id = co_await repo.list_events_for_turn(core::TurnId{});
    REQUIRE_FALSE(zero_id.has_value());
    REQUIRE(zero_id.error().kind() == core::ErrorKind::invalid_argument);

    auto zero_limit = co_await repo.list_events_for_turn(turn_id_with(0x10), 0);
    REQUIRE_FALSE(zero_limit.has_value());
    REQUIRE(zero_limit.error().kind() == core::ErrorKind::invalid_argument);
  });
}
