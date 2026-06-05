// tests/memory/test_longterm.cpp — long-term memory contract coverage.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
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

memory::longterm::VectorEmbedding make_embedding() {
  return memory::longterm::VectorEmbedding{
      .model = "test-embedding-v1",
      .values = {0.25F, -0.5F, 0.75F},
  };
}

class FakeBackend final : public memory::longterm::Backend {
public:
  explicit FakeBackend(memory::longterm::Record record) : record_{std::move(record)} {}

  [[nodiscard]] async::Awaitable<core::Result<memory::longterm::Record>> get(memory::longterm::RecordKey key) override {
    last_key = std::move(key);
    co_return record_;
  }

  [[nodiscard]] async::Awaitable<core::Result<std::vector<memory::longterm::SearchHit>>>
  search(memory::longterm::Query query, std::size_t limit) override {
    last_query = std::move(query);
    last_limit = limit;
    co_return std::vector<memory::longterm::SearchHit>{memory::longterm::SearchHit{
        .record = record_,
        .score = 0.9,
        .lexical_score = 0.8,
        .vector_score = 0.7,
    }};
  }

  [[nodiscard]] async::Awaitable<core::Result<memory::longterm::Record>>
  upsert(memory::longterm::WriteRequest request) override {
    record_ = std::move(request.record);
    co_return record_;
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> remove(memory::longterm::RecordKey key) override {
    last_key = std::move(key);
    co_return core::Result<void>{};
  }

  memory::longterm::RecordKey last_key;
  memory::longterm::Query last_query;
  std::size_t last_limit{};

private:
  memory::longterm::Record record_;
};

class FakeVectorBackend final : public memory::longterm::VectorBackend {
public:
  [[nodiscard]] async::Awaitable<core::Result<void>> upsert(memory::longterm::VectorUpsert request) override {
    last_key = std::move(request.key);
    co_return core::Result<void>{};
  }

  [[nodiscard]] async::Awaitable<core::Result<std::vector<memory::longterm::VectorHit>>>
  search(memory::longterm::VectorSearchQuery query, std::size_t limit) override {
    last_scope_key = std::move(query.scope_key);
    last_limit = limit;
    co_return std::vector<memory::longterm::VectorHit>{memory::longterm::VectorHit{
        .key = memory::longterm::RecordKey{.id = "rec-1", .scope_key = last_scope_key},
        .score = 0.95,
    }};
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> remove(memory::longterm::VectorRemoveRequest request) override {
    last_key = std::move(request.key);
    co_return core::Result<void>{};
  }

  memory::longterm::RecordKey last_key;
  std::string last_scope_key;
  std::size_t last_limit{};
};

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

TEST_CASE("longterm vector validation pins sqlite-vec adapter inputs", "[unit][memory][longterm]") {
  auto embedding = make_embedding();
  REQUIRE(memory::longterm::validate_embedding(embedding).has_value());
  REQUIRE(memory::longterm::validate_vector_upsert(
              memory::longterm::VectorUpsert{
                  .key = memory::longterm::RecordKey{.id = "rec-1", .scope_key = "agent:coder"},
                  .embedding = embedding,
              })
              .has_value());
  REQUIRE(memory::longterm::validate_vector_search_query(
              memory::longterm::VectorSearchQuery{
                  .scope_key = "agent:coder",
                  .embedding = embedding,
                  .kinds = {memory::longterm::RecordKind::project},
              },
              10)
              .has_value());

  embedding.values.push_back(std::numeric_limits<float>::infinity());
  auto invalid = memory::longterm::validate_embedding(embedding);
  REQUIRE_FALSE(invalid.has_value());
  REQUIRE(invalid.error().kind() == core::ErrorKind::invalid_argument);
}

TEST_CASE("longterm backend interfaces compose through async contracts", "[unit][memory][longterm]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    FakeBackend backend{make_record()};

    auto fetched = co_await backend.get(memory::longterm::RecordKey{.id = "rec-1", .scope_key = "agent:coder"});
    REQUIRE(fetched.has_value());
    REQUIRE(fetched->key.id == "rec-1");

    auto hits = co_await backend.search(
        memory::longterm::Query{
            .scope_key = "agent:coder",
            .text = "workflow",
            .kinds = {},
        },
        5);
    REQUIRE(hits.has_value());
    REQUIRE(hits->size() == 1);
    REQUIRE((*hits)[0].score == 0.9);
    REQUIRE(backend.last_limit == 5);

    auto upserted = co_await backend.upsert(memory::longterm::WriteRequest{.record = make_record()});
    REQUIRE(upserted.has_value());

    auto removed = co_await backend.remove(memory::longterm::RecordKey{.id = "rec-1", .scope_key = "agent:coder"});
    REQUIRE(removed.has_value());
  });
}

TEST_CASE("longterm vector backend interface composes through async contracts", "[unit][memory][longterm]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    FakeVectorBackend backend;

    auto upserted = co_await backend.upsert(memory::longterm::VectorUpsert{
        .key = memory::longterm::RecordKey{.id = "rec-1", .scope_key = "agent:coder"},
        .embedding = make_embedding(),
    });
    REQUIRE(upserted.has_value());

    auto hits = co_await backend.search(
        memory::longterm::VectorSearchQuery{
            .scope_key = "agent:coder",
            .embedding = make_embedding(),
            .kinds = {},
        },
        3);
    REQUIRE(hits.has_value());
    REQUIRE(hits->size() == 1);
    REQUIRE((*hits)[0].key.scope_key == "agent:coder");
    REQUIRE(backend.last_limit == 3);

    auto removed = co_await backend.remove(memory::longterm::VectorRemoveRequest{
        .key = memory::longterm::RecordKey{.id = "rec-1", .scope_key = "agent:coder"},
    });
    REQUIRE(removed.has_value());
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
    REQUIRE((*hits)[0].lexical_score.has_value());
    REQUIRE_FALSE((*hits)[0].vector_score.has_value());

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
