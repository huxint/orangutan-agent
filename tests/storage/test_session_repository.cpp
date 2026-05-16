// tests/storage/test_session_repository.cpp — sessions domain repository coverage.

#include <array>
#include <chrono>
#include <cstddef>
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

}  // namespace

TEST_CASE("SessionRepository::migrate applies the sessions schema once", "[unit][storage][session_repository]") {
  TempDb db{"oran-session-repo-migrate"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::SessionRepository repo{pool};

    auto first = co_await repo.migrate();
    REQUIRE(first.has_value());
    REQUIRE(first->previous_version == 0);
    REQUIRE(first->current_version == 1);
    REQUIRE(first->applied_versions == std::vector<std::int64_t>{1});

    auto second = co_await repo.migrate();
    REQUIRE(second.has_value());
    REQUIRE(second->previous_version == 1);
    REQUIRE(second->current_version == 1);
    REQUIRE(second->applied_versions.empty());
  });
}

TEST_CASE("SessionRepository::migrate accepts an explicit migration directory", "[unit][storage][session_repository]") {
  TempDb db{"oran-session-repo-migrate-dir"};
  TempDir migrations{"oran-session-repo-migrations"};
  write_file(migrations.path() / "0001-custom-marker.sql",
             "CREATE TABLE custom_sessions_marker(id INTEGER PRIMARY KEY)");

  test::run_async([&db, &migrations](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::SessionRepository repo{
        pool,
        storage::SessionRepositoryOptions{.migrations_directory = migrations.string()},
    };

    auto report = co_await repo.migrate();
    REQUIRE(report.has_value());
    REQUIRE(report->current_version == 1);
    REQUIRE(report->applied_versions == std::vector<std::int64_t>{1});

    auto reader = co_await pool.acquire_reader();
    REQUIRE(reader.has_value());
    auto marker = reader->connection().query(
        "SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'custom_sessions_marker'");
    REQUIRE(marker.has_value());
    REQUIRE(marker->rows.size() == 1);
  });
}

TEST_CASE("SessionRepository append_message and load_messages round-trip ordered rows",
          "[unit][storage][session_repository]") {
  TempDb db{"oran-session-repo-roundtrip"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::SessionRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    auto first = co_await repo.append_message(storage::AppendSessionMessageRequest{
        .session_id = "s-1",
        .agent_key = "coder",
        .role = core::Role::user,
        .content_json = R"json({"text":"hello"})json",
    });
    REQUIRE(first.has_value());
    REQUIRE(first->sequence == 1);
    REQUIRE(first->metadata_json == "{}");

    auto second = co_await repo.append_message(storage::AppendSessionMessageRequest{
        .session_id = "s-1",
        .agent_key = "coder",
        .role = core::Role::assistant,
        .content_json = R"json({"text":"hi"})json",
        .metadata_json = R"json({"source":"test"})json",
    });
    REQUIRE(second.has_value());
    REQUIRE(second->sequence == 2);

    auto loaded = co_await repo.load_messages(storage::SessionKey{.session_id = "s-1", .agent_key = "coder"});
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->size() == 2);
    REQUIRE((*loaded)[0].sequence == 1);
    REQUIRE((*loaded)[0].role == core::Role::user);
    REQUIRE((*loaded)[0].content_json == R"json({"text":"hello"})json");
    REQUIRE((*loaded)[1].sequence == 2);
    REQUIRE((*loaded)[1].role == core::Role::assistant);
    REQUIRE((*loaded)[1].metadata_json == R"json({"source":"test"})json");

    auto session = co_await repo.get_session(storage::SessionKey{.session_id = "s-1", .agent_key = "coder"});
    REQUIRE(session.has_value());
    REQUIRE(session->has_value());
    REQUIRE((*session)->session_id == "s-1");
    REQUIRE((*session)->agent_key == "coder");
    REQUIRE((*session)->message_count == 2);
    REQUIRE((*session)->metadata_json == "{}");
  });
}

TEST_CASE("SessionRepository list_sessions is scoped by agent and honors limits",
          "[unit][storage][session_repository]") {
  TempDb db{"oran-session-repo-list"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::SessionRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    auto coder_a_append = co_await repo.append_message(storage::AppendSessionMessageRequest{
        .session_id = "s-a",
        .agent_key = "coder",
        .role = core::Role::user,
        .content_json = R"json({"text":"a"})json",
    });
    REQUIRE(coder_a_append.has_value());
    auto coder_b_append = co_await repo.append_message(storage::AppendSessionMessageRequest{
        .session_id = "s-b",
        .agent_key = "coder",
        .role = core::Role::assistant,
        .content_json = R"json({"text":"b"})json",
    });
    REQUIRE(coder_b_append.has_value());
    auto researcher_append = co_await repo.append_message(storage::AppendSessionMessageRequest{
        .session_id = "s-a",
        .agent_key = "researcher",
        .role = core::Role::user,
        .content_json = R"json({"text":"other"})json",
    });
    REQUIRE(researcher_append.has_value());

    auto coder_a = co_await repo.load_messages(storage::SessionKey{.session_id = "s-a", .agent_key = "coder"});
    REQUIRE(coder_a.has_value());
    REQUIRE(coder_a->size() == 1);
    auto researcher_a =
        co_await repo.load_messages(storage::SessionKey{.session_id = "s-a", .agent_key = "researcher"});
    REQUIRE(researcher_a.has_value());
    REQUIRE(researcher_a->size() == 1);

    auto coder = co_await repo.list_sessions(storage::ListSessionsOptions{.agent_key = "coder", .limit = 10});
    REQUIRE(coder.has_value());
    REQUIRE(coder->size() == 2);
    for (const auto& session : *coder) {
      REQUIRE(session.agent_key == "coder");
      REQUIRE(session.message_count == 1);
    }

    auto limited = co_await repo.list_sessions(storage::ListSessionsOptions{.agent_key = "coder", .limit = 1});
    REQUIRE(limited.has_value());
    REQUIRE(limited->size() == 1);

    auto researcher = co_await repo.list_sessions(storage::ListSessionsOptions{.agent_key = "researcher", .limit = 10});
    REQUIRE(researcher.has_value());
    REQUIRE(researcher->size() == 1);
    REQUIRE((*researcher)[0].session_id == "s-a");
    REQUIRE((*researcher)[0].agent_key == "researcher");
  });
}

TEST_CASE("SessionRepository round-trips every core::Role enumerator", "[unit][storage][session_repository]") {
  TempDb db{"oran-session-repo-role-enum"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::SessionRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    const std::array<core::Role, 4> roles{core::Role::user,
                                          core::Role::assistant,
                                          core::Role::system,
                                          core::Role::tool};
    for (auto role : roles) {
      auto appended = co_await repo.append_message(storage::AppendSessionMessageRequest{
          .session_id = "s-roles",
          .agent_key = "coder",
          .role = role,
          .content_json = R"json({"text":"x"})json",
      });
      REQUIRE(appended.has_value());
      REQUIRE(appended->role == role);
    }

    auto loaded = co_await repo.load_messages(storage::SessionKey{.session_id = "s-roles", .agent_key = "coder"});
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->size() == roles.size());
    for (std::size_t i = 0; i < roles.size(); ++i) {
      REQUIRE((*loaded)[i].role == roles[i]);
    }
  });
}

TEST_CASE("SessionRepository surfaces a storage error for rows with unknown role text",
          "[unit][storage][session_repository]") {
  TempDb db{"oran-session-repo-bad-role"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::SessionRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    {
      auto writer = co_await pool.acquire_writer();
      REQUIRE(writer.has_value());
      // Bypass the repository to inject a row whose role text is outside
      // `core::Role`. The repository must reject it on read rather than
      // silently coerce.
      auto inserted = writer->connection().execute(
          R"sql(
INSERT INTO session_messages(session_id, agent_key, sequence, role, content_json, metadata_json, created_at)
VALUES ('s-bad', 'coder', 1, 'sidekick', '{}', '{}', '2026-05-16T00:00:00.000Z')
)sql");
      REQUIRE(inserted.has_value());
    }

    auto loaded = co_await repo.load_messages(storage::SessionKey{.session_id = "s-bad", .agent_key = "coder"});
    REQUIRE_FALSE(loaded.has_value());
    REQUIRE(loaded.error().kind() == core::ErrorKind::storage);
  });
}

TEST_CASE("SessionRepository returns empty results for missing sessions", "[unit][storage][session_repository]") {
  TempDb db{"oran-session-repo-missing"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::SessionRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    auto loaded = co_await repo.load_messages(storage::SessionKey{.session_id = "missing", .agent_key = "coder"});
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->empty());

    auto session = co_await repo.get_session(storage::SessionKey{.session_id = "missing", .agent_key = "coder"});
    REQUIRE(session.has_value());
    REQUIRE_FALSE(session->has_value());
  });
}

TEST_CASE("SessionRepository validates required fields", "[unit][storage][session_repository]") {
  TempDb db{"oran-session-repo-invalid"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::SessionRepository repo{pool};

    auto append = co_await repo.append_message(storage::AppendSessionMessageRequest{
        .session_id = "",
        .agent_key = "coder",
        .role = core::Role::user,
        .content_json = "{}",
    });
    REQUIRE_FALSE(append.has_value());
    REQUIRE(append.error().kind() == core::ErrorKind::invalid_argument);

    auto load = co_await repo.load_messages(storage::SessionKey{.session_id = "s-1", .agent_key = ""});
    REQUIRE_FALSE(load.has_value());
    REQUIRE(load.error().kind() == core::ErrorKind::invalid_argument);

    auto list_agent = co_await repo.list_sessions(storage::ListSessionsOptions{.agent_key = "", .limit = 10});
    REQUIRE_FALSE(list_agent.has_value());
    REQUIRE(list_agent.error().kind() == core::ErrorKind::invalid_argument);

    auto list_limit = co_await repo.list_sessions(storage::ListSessionsOptions{.agent_key = "coder", .limit = 0});
    REQUIRE_FALSE(list_limit.has_value());
    REQUIRE(list_limit.error().kind() == core::ErrorKind::invalid_argument);
  });
}
