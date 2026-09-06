// tests/memory/test_longterm.cpp — long-term memory contract coverage.

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/memory.hpp>
#include <oran/storage.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace memory = orangutan::memory;
namespace storage = orangutan::storage;
namespace test = orangutan::tests;

namespace {

using namespace std::chrono_literals;

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

storage::Pool open_pool(asio::io_context& io, TempDb& db) {
  auto pool =
      storage::Pool::open(io.get_executor(),
                          storage::PoolOptions{.path = db.string(), .reader_count = 2, .statement_cache_capacity = 16});
  REQUIRE(pool.has_value());
  return std::move(*pool);
}

memory::longterm::Record make_record() {
  const auto created = core::Time{core::Time::time_point{1s}};
  const auto updated = core::Time{core::Time::time_point{2s}};
  return memory::longterm::Record{
      .key = memory::longterm::RecordKey{.id = "rec-1", .scope_key = "agent:coder"},
      .kind = memory::longterm::RecordKind::project,
      .title = "Build notes",
      .body = "The repository prefers scoped slices.\nDocs move with code.",
      .created_at = created,
      .updated_at = updated,
      .last_read_at = updated,
      .importance = 0.7,
      .tags = {"repo", "workflow"},
      .linked_record_ids = {"rec-0"},
  };
}

memory::longterm::Record make_record(std::string id,
                                     std::string scope_key,
                                     memory::longterm::RecordKind kind,
                                     std::string title,
                                     std::string body) {
  auto record = make_record();
  record.key.id = std::move(id);
  record.key.scope_key = std::move(scope_key);
  record.kind = kind;
  record.title = std::move(title);
  record.body = std::move(body);
  record.tags = {};
  record.linked_record_ids = {};
  return record;
}

}  // namespace

TEST_CASE("longterm::RecordKind uses reflection-backed wire spelling", "[unit][memory][longterm]") {
  using memory::longterm::RecordKind;

  REQUIRE(core::enum_name(RecordKind::user) == "user");
  REQUIRE(core::enum_name(RecordKind::feedback) == "feedback");
  REQUIRE(core::enum_name(RecordKind::project) == "project");
  REQUIRE(core::enum_name(RecordKind::reference) == "reference");
  REQUIRE(core::enum_name(RecordKind::team) == "team");
  REQUIRE(core::parse_enum<RecordKind>("project") == RecordKind::project);
  REQUIRE_FALSE(core::parse_enum<RecordKind>("Project").has_value());
}

TEST_CASE("longterm validation accepts well-shaped record and query contracts", "[unit][memory][longterm]") {
  auto record = make_record();
  REQUIRE(memory::longterm::validate_record(record).has_value());
  REQUIRE(memory::longterm::validate_write_request(memory::longterm::WriteRequest{.record = record}).has_value());
  REQUIRE(memory::longterm::validate_touch_request(memory::longterm::TouchRequest{.key = record.key}).has_value());
  auto query = memory::longterm::Query{
      .scope_key = "agent:coder",
      .text = "scoped slices",
      .kinds = {memory::longterm::RecordKind::project, memory::longterm::RecordKind::reference},
  };
  REQUIRE(memory::longterm::validate_query(query, 10).has_value());
}

TEST_CASE("longterm validation rejects malformed record fields", "[unit][memory][longterm]") {
  auto record = make_record();
  record.key.id.clear();
  auto missing_id = memory::longterm::validate_record(record);
  REQUIRE_FALSE(missing_id.has_value());
  REQUIRE(missing_id.error().kind() == core::ErrorKind::invalid_argument);

  record = make_record();
  record.importance = std::numeric_limits<double>::quiet_NaN();
  auto bad_importance = memory::longterm::validate_record(record);
  REQUIRE_FALSE(bad_importance.has_value());
  REQUIRE(bad_importance.error().kind() == core::ErrorKind::invalid_argument);

  record = make_record();
  record.tags.push_back("repo");
  auto duplicate_tag = memory::longterm::validate_record(record);
  REQUIRE_FALSE(duplicate_tag.has_value());
  REQUIRE(duplicate_tag.error().kind() == core::ErrorKind::invalid_argument);

  record = make_record();
  record.updated_at = core::Time{core::Time::time_point{}};
  auto bad_time = memory::longterm::validate_record(record);
  REQUIRE_FALSE(bad_time.has_value());
  REQUIRE(bad_time.error().kind() == core::ErrorKind::invalid_argument);

  record = make_record();
  record.body = std::string{"ok\0bad", 6};
  auto bad_body = memory::longterm::validate_record(record);
  REQUIRE_FALSE(bad_body.has_value());
  REQUIRE(bad_body.error().kind() == core::ErrorKind::invalid_argument);
}

TEST_CASE("longterm validation rejects malformed search contracts", "[unit][memory][longterm]") {
  auto query = memory::longterm::Query{
      .scope_key = "agent:coder",
      .text = "",
      .kinds = {},
  };
  auto blank_query = memory::longterm::validate_query(query, 10);
  REQUIRE_FALSE(blank_query.has_value());
  REQUIRE(blank_query.error().kind() == core::ErrorKind::invalid_argument);

  query.text = "project notes";
  query.kinds = {memory::longterm::RecordKind::project, memory::longterm::RecordKind::project};
  auto duplicate_kind = memory::longterm::validate_query(query, 10);
  REQUIRE_FALSE(duplicate_kind.has_value());
  REQUIRE(duplicate_kind.error().kind() == core::ErrorKind::invalid_argument);

  query.kinds = {};
  auto zero_limit = memory::longterm::validate_query(query, 0);
  REQUIRE_FALSE(zero_limit.has_value());
  REQUIRE(zero_limit.error().kind() == core::ErrorKind::invalid_argument);

}

TEST_CASE("longterm recall rejects invalid queries before storage access", "[unit][memory][longterm][recall]") {
  TempDb db{"oran-memory-invalid-recall"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    memory::longterm::Fts5Backend backend{pool};

    auto result = co_await memory::longterm::recall(backend, memory::longterm::RecallRequest{
        .query = memory::longterm::Query{.scope_key = "agent:coder", .text = "", .kinds = {}},
        .limit = 5,
    });

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  });
}

TEST_CASE("longterm recall framing follows record content", "[unit][memory][longterm][recall]") {
  const auto hits = std::array{memory::longterm::SearchHit{.record = make_record(), .score = 0.9}};

  const auto framing = memory::longterm::render_recall_framing(hits);

  REQUIRE(framing.section_text == "Long-term memory:\n"
                                   "- [project] Build notes (id: rec-1)\n"
                                   "  The repository prefers scoped slices. Docs move with code.\n"
                                   "  tags: repo, workflow\n"
                                   "  linked: rec-0\n");
}

TEST_CASE("longterm recall persists read timestamps within the query scope", "[unit][memory][longterm][recall]") {
  TempDb db{"oran-memory-recall-touch"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    memory::longterm::Fts5Backend backend{pool};
    auto migrated = co_await backend.migrate();
    REQUIRE(migrated.has_value());
    const auto record = make_record();
    auto other = record;
    other.key.scope_key = "agent:researcher";
    auto saved = co_await backend.upsert({.record = record});
    REQUIRE(saved.has_value());
    auto saved_other = co_await backend.upsert({.record = other});
    REQUIRE(saved_other.has_value());

    auto result = co_await memory::longterm::recall(backend, memory::longterm::RecallRequest{
        .query = memory::longterm::Query{.scope_key = "agent:coder", .text = "workflow", .kinds = {}},
        .limit = 5,
    });

    REQUIRE(result.has_value());
    REQUIRE(result->hits.size() == 1);
    REQUIRE(result->hits[0].record.key == record.key);
    REQUIRE(result->hits[0].record.last_read_at > record.last_read_at);
    auto fetched = co_await backend.get(record.key);
    REQUIRE(fetched.has_value());
    REQUIRE(fetched->last_read_at == result->hits[0].record.last_read_at);
    REQUIRE(fetched->updated_at == record.updated_at);
    auto untouched = co_await backend.get(other.key);
    REQUIRE(untouched.has_value());
    REQUIRE(*untouched == other);
  });
}

TEST_CASE("longterm recall data_json preserves record and score fields", "[unit][memory][longterm][recall]") {
  const auto hits = std::array{memory::longterm::SearchHit{.record = make_record(), .score = 0.9}};

  const auto data_json = memory::longterm::render_recall_data_json(hits);

  REQUIRE(data_json.contains(R"("kind":"memory_recall")"));
  REQUIRE(data_json.contains(R"("match_count":1)"));
  REQUIRE(data_json.contains(R"("id":"rec-1")"));
  REQUIRE(data_json.contains(R"("scope_key":"agent:coder")"));
  REQUIRE(data_json.contains(R"("created_at":"1970-01-01T00:00:01.000Z")"));
  REQUIRE(data_json.contains(R"("score":0.9)"));
  REQUIRE(data_json.contains(R"("lexical_score":0.9)"));
  REQUIRE(data_json.contains(R"("vector_score":null)"));
}

TEST_CASE("longterm remember data_json carries saved record metadata", "[unit][memory][longterm][recall]") {
  auto record = make_record();
  record.shadow = true;

  const auto data_json = memory::longterm::render_remember_data_json(record);
  REQUIRE(data_json.contains(R"("kind":"memory_remember")"));
  REQUIRE(data_json.contains(R"("id":"rec-1")"));
  REQUIRE(data_json.contains(R"("scope_key":"agent:coder")"));
  REQUIRE(data_json.contains(R"("kind":"project")"));
  REQUIRE(data_json.contains(R"("created_at":"1970-01-01T00:00:01.000Z")"));
  REQUIRE(data_json.contains(R"("tags":["repo","workflow"])"));
  REQUIRE(data_json.contains(R"("linked_record_ids":["rec-0"])"));
  REQUIRE(data_json.contains(R"("shadow":true)"));
  REQUIRE_FALSE(data_json.contains(R"("score")"));
}

TEST_CASE("longterm forget data_json carries scoped removed key", "[unit][memory][longterm][recall]") {
  const auto key = memory::longterm::RecordKey{.id = "rec-1", .scope_key = "agent:coder"};

  const auto data_json = memory::longterm::render_forget_data_json(key);
  REQUIRE(data_json.contains(R"("kind":"memory_forget")"));
  REQUIRE(data_json.contains(R"("id":"rec-1")"));
  REQUIRE(data_json.contains(R"("scope_key":"agent:coder")"));
  REQUIRE_FALSE(data_json.contains(R"("title")"));
}

TEST_CASE("longterm recall returns empty framing for no matches", "[unit][memory][longterm][recall]") {
  TempDb db{"oran-memory-empty-recall"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    memory::longterm::Fts5Backend backend{pool};
    auto migrated = co_await backend.migrate();
    REQUIRE(migrated.has_value());

    auto result = co_await memory::longterm::recall(backend, memory::longterm::RecallRequest{
        .query = memory::longterm::Query{.scope_key = "agent:coder", .text = "workflow", .kinds = {}},
        .limit = 5,
    });

    REQUIRE(result.has_value());
    REQUIRE(result->hits.empty());
    REQUIRE(result->framing.section_text.empty());
  });
}

TEST_CASE("longterm::Fts5Backend migrates the lexical memory schema", "[unit][memory][longterm][fts5]") {
  TempDb db{"oran-memory-longterm-migrate"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    memory::longterm::Fts5Backend backend{pool};

    auto first = co_await backend.migrate();
    REQUIRE(first.has_value());
    REQUIRE(first->previous_version == 0);
    REQUIRE(first->current_version == 1);
    REQUIRE(first->applied_versions == std::vector<std::int64_t>{1});

    auto second = co_await backend.migrate();
    REQUIRE(second.has_value());
    REQUIRE(second->previous_version == 1);
    REQUIRE(second->current_version == 1);
    REQUIRE(second->applied_versions.empty());
  });
}

TEST_CASE("longterm reopening preserves records and unrelated database content", "[unit][memory][longterm][fts5]") {
  TempDb db{"oran-memory-reopen"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto record = make_record();
    record.kind = memory::longterm::RecordKind::team;
    record.shadow = true;
    std::vector<storage::ColumnValue> migration_row;
    {
      auto pool = open_pool(io, db);
      memory::longterm::Fts5Backend backend{pool};
      auto migrated = co_await backend.migrate();
      REQUIRE(migrated.has_value());
      auto saved = co_await backend.upsert({.record = record});
      REQUIRE(saved.has_value());
      auto writer = co_await pool.acquire_writer();
      REQUIRE(writer.has_value());
      auto& connection = writer->connection();
      auto created = connection.execute("CREATE TABLE host_metadata(key TEXT PRIMARY KEY, value BLOB)");
      REQUIRE(created.has_value());
      auto inserted = connection.execute("INSERT INTO host_metadata VALUES ('keep', X'0001FF')");
      REQUIRE(inserted.has_value());
      auto versions = connection.query("SELECT version, name, applied_at FROM schema_versions ORDER BY version");
      REQUIRE(versions.has_value());
      REQUIRE(versions->rows.size() == 1);
      migration_row = versions->rows.front().values;
    }

    auto pool = open_pool(io, db);
    memory::longterm::Fts5Backend backend{pool};
    auto reopened = co_await backend.migrate();

    REQUIRE(reopened.has_value());
    REQUIRE(reopened->previous_version == 1);
    REQUIRE(reopened->current_version == 1);
    REQUIRE(reopened->applied_versions.empty());
    auto fetched = co_await backend.get(record.key);
    REQUIRE(fetched.has_value());
    REQUIRE(*fetched == record);
    auto reader = co_await pool.acquire_reader();
    REQUIRE(reader.has_value());
    auto& connection = reader->connection();
    auto metadata = connection.query("SELECT key, hex(value) FROM host_metadata");
    REQUIRE(metadata.has_value());
    REQUIRE(metadata->rows.size() == 1);
    REQUIRE(metadata->rows.front().values == std::vector<storage::ColumnValue>{"keep", "0001FF"});
    auto versions = connection.query("SELECT version, name, applied_at FROM schema_versions ORDER BY version");
    REQUIRE(versions.has_value());
    REQUIRE(versions->rows.size() == 1);
    REQUIRE(versions->rows.front().values == migration_row);
  });
}

TEST_CASE("longterm::Fts5Backend upserts, gets, and searches scoped records", "[unit][memory][longterm][fts5]") {
  TempDb db{"oran-memory-longterm-search"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    memory::longterm::Fts5Backend backend{pool};
    auto migrated = co_await backend.migrate();
    REQUIRE(migrated.has_value());

    auto coder = make_record("rec-coder",
                             "agent:coder",
                             memory::longterm::RecordKind::project,
                             "Scoped slices",
                             "Orangutan keeps implementation slices small and documented.");
    coder.tags = {"workflow", "orangutan"};
    coder.linked_record_ids = {"rec-prev"};
    auto researcher = make_record("rec-researcher",
                                  "agent:researcher",
                                  memory::longterm::RecordKind::project,
                                  "Scoped slices",
                                  "Researcher notes also mention Orangutan slices.");

    auto inserted_coder = co_await backend.upsert(memory::longterm::WriteRequest{.record = coder});
    REQUIRE(inserted_coder.has_value());
    auto inserted_researcher = co_await backend.upsert(memory::longterm::WriteRequest{.record = researcher});
    REQUIRE(inserted_researcher.has_value());

    auto fetched = co_await backend.get(memory::longterm::RecordKey{.id = "rec-coder", .scope_key = "agent:coder"});
    REQUIRE(fetched.has_value());
    REQUIRE(fetched->title == "Scoped slices");
    REQUIRE(fetched->tags == std::vector<std::string>{"workflow", "orangutan"});
    REQUIRE(fetched->linked_record_ids == std::vector<std::string>{"rec-prev"});

    auto hits = co_await backend.search(
        memory::longterm::Query{
            .scope_key = "agent:coder",
            .text = "orangutan",
            .kinds = {},
        },
        10);
    REQUIRE(hits.has_value());
    REQUIRE(hits->size() == 1);
    REQUIRE((*hits)[0].record.key.id == "rec-coder");

    auto other_scope = co_await backend.search(
        memory::longterm::Query{
            .scope_key = "agent:researcher",
            .text = "orangutan",
            .kinds = {},
        },
        10);
    REQUIRE(other_scope.has_value());
    REQUIRE(other_scope->size() == 1);
    REQUIRE((*other_scope)[0].record.key.id == "rec-researcher");
  });
}

TEST_CASE("longterm::Fts5Backend applies kind and shadow filters", "[unit][memory][longterm][fts5]") {
  TempDb db{"oran-memory-longterm-filters"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    memory::longterm::Fts5Backend backend{pool};
    auto migrated = co_await backend.migrate();
    REQUIRE(migrated.has_value());

    auto visible_project = make_record("project-visible",
                                       "agent:coder",
                                       memory::longterm::RecordKind::project,
                                       "Plan",
                                       "FTS5 lexical search should find this banana marker.");
    auto visible_user = make_record("user-visible",
                                    "agent:coder",
                                    memory::longterm::RecordKind::user,
                                    "Preference",
                                    "The user also wrote a banana marker.");
    auto shadow_project = make_record("project-shadow",
                                      "agent:coder",
                                      memory::longterm::RecordKind::project,
                                      "Old plan",
                                      "A shadow banana marker should stay hidden by default.");
    shadow_project.shadow = true;

    REQUIRE((co_await backend.upsert(memory::longterm::WriteRequest{.record = visible_project})).has_value());
    REQUIRE((co_await backend.upsert(memory::longterm::WriteRequest{.record = visible_user})).has_value());
    REQUIRE((co_await backend.upsert(memory::longterm::WriteRequest{.record = shadow_project})).has_value());

    auto project_hits = co_await backend.search(
        memory::longterm::Query{
            .scope_key = "agent:coder",
            .text = "banana",
            .kinds = {memory::longterm::RecordKind::project},
        },
        10);
    REQUIRE(project_hits.has_value());
    REQUIRE(project_hits->size() == 1);
    REQUIRE((*project_hits)[0].record.key.id == "project-visible");

    auto including_shadow = co_await backend.search(
        memory::longterm::Query{
            .scope_key = "agent:coder",
            .text = "banana",
            .kinds = {memory::longterm::RecordKind::project},
            .include_shadow = true,
        },
        10);
    REQUIRE(including_shadow.has_value());
    REQUIRE(including_shadow->size() == 2);
  });
}

TEST_CASE("longterm::Fts5Backend updates and removes indexed rows", "[unit][memory][longterm][fts5]") {
  TempDb db{"oran-memory-longterm-update-remove"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    memory::longterm::Fts5Backend backend{pool};
    auto migrated = co_await backend.migrate();
    REQUIRE(migrated.has_value());

    auto record = make_record("rec-1",
                              "agent:coder",
                              memory::longterm::RecordKind::reference,
                              "Original",
                              "The original indexed word is kumquat.");
    REQUIRE((co_await backend.upsert(memory::longterm::WriteRequest{.record = record})).has_value());

    auto old_hits = co_await backend.search(
        memory::longterm::Query{
            .scope_key = "agent:coder",
            .text = "kumquat",
            .kinds = {},
        },
        10);
    REQUIRE(old_hits.has_value());
    REQUIRE(old_hits->size() == 1);

    record.title = "Updated";
    record.body = "The replacement indexed word is persimmon.";
    REQUIRE((co_await backend.upsert(memory::longterm::WriteRequest{.record = record})).has_value());

    auto stale_hits = co_await backend.search(
        memory::longterm::Query{
            .scope_key = "agent:coder",
            .text = "kumquat",
            .kinds = {},
        },
        10);
    REQUIRE(stale_hits.has_value());
    REQUIRE(stale_hits->empty());

    auto fresh_hits = co_await backend.search(
        memory::longterm::Query{
            .scope_key = "agent:coder",
            .text = "persimmon",
            .kinds = {},
        },
        10);
    REQUIRE(fresh_hits.has_value());
    REQUIRE(fresh_hits->size() == 1);
    REQUIRE((*fresh_hits)[0].record.title == "Updated");

    auto removed = co_await backend.remove(memory::longterm::RecordKey{.id = "rec-1", .scope_key = "agent:coder"});
    REQUIRE(removed.has_value());
    auto fetched = co_await backend.get(memory::longterm::RecordKey{.id = "rec-1", .scope_key = "agent:coder"});
    REQUIRE_FALSE(fetched.has_value());
    REQUIRE(fetched.error().kind() == core::ErrorKind::not_found);
    auto after_remove = co_await backend.search(
        memory::longterm::Query{
            .scope_key = "agent:coder",
            .text = "persimmon",
            .kinds = {},
        },
        10);
    REQUIRE(after_remove.has_value());
    REQUIRE(after_remove->empty());
  });
}

TEST_CASE("longterm::Fts5Backend touches last_read_at without rebuilding indexed text",
          "[unit][memory][longterm][fts5]") {
  TempDb db{"oran-memory-longterm-touch"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    memory::longterm::Fts5Backend backend{pool};
    auto migrated = co_await backend.migrate();
    REQUIRE(migrated.has_value());

    auto record = make_record("rec-touch",
                              "agent:coder",
                              memory::longterm::RecordKind::reference,
                              "Touch metadata",
                              "Recall touch preserves the indexed apricot text.");
    const auto original_read_at = record.last_read_at;
    REQUIRE((co_await backend.upsert(memory::longterm::WriteRequest{.record = record})).has_value());

    const auto touched_at = core::Time{core::Time::time_point{10s}};
    auto touched = co_await backend.touch(memory::longterm::TouchRequest{.key = record.key, .read_at = touched_at});
    REQUIRE(touched.has_value());
    REQUIRE(touched->last_read_at == touched_at);
    REQUIRE(touched->updated_at == record.updated_at);

    auto fetched = co_await backend.get(record.key);
    REQUIRE(fetched.has_value());
    REQUIRE(fetched->last_read_at == touched_at);

    auto regressed =
        co_await backend.touch(memory::longterm::TouchRequest{.key = record.key, .read_at = original_read_at});
    REQUIRE(regressed.has_value());
    REQUIRE(regressed->last_read_at == touched_at);

    auto hits = co_await backend.search(
        memory::longterm::Query{
            .scope_key = "agent:coder",
            .text = "apricot",
            .kinds = {},
        },
        10);
    REQUIRE(hits.has_value());
    REQUIRE(hits->size() == 1);
    REQUIRE((*hits)[0].record.key.id == "rec-touch");
    REQUIRE((*hits)[0].record.last_read_at == touched_at);
  });
}

TEST_CASE("longterm recall returns indexed records and prompt framing", "[unit][memory][longterm][recall][fts5]") {
  TempDb db{"oran-memory-recall-fts5"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    memory::longterm::Fts5Backend backend{pool};
    auto migrated = co_await backend.migrate();
    REQUIRE(migrated.has_value());

    auto record = make_record("recall-rec",
                              "agent:coder",
                              memory::longterm::RecordKind::reference,
                              "Scoped recall",
                              "Scoped recall composes over the FTS5 backend.");
    record.tags = {"recall", "fts5"};
    REQUIRE((co_await backend.upsert(memory::longterm::WriteRequest{.record = record})).has_value());

    auto result = co_await memory::longterm::recall(backend, memory::longterm::RecallRequest{
        .query =
            memory::longterm::Query{
                .scope_key = "agent:coder",
                .text = "recall",
                .kinds = {memory::longterm::RecordKind::reference},
            },
        .limit = 3,
    });

    REQUIRE(result.has_value());
    REQUIRE(result->hits.size() == 1);
    REQUIRE(result->hits[0].record.key.id == "recall-rec");
    REQUIRE(result->framing.section_text.contains("Scoped recall"));
    REQUIRE(result->framing.section_text.contains("tags: recall, fts5"));
  });
}
