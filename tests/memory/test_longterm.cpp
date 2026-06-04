// tests/memory/test_longterm.cpp — long-term memory contract coverage.

#include <chrono>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/memory.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace memory = orangutan::memory;
namespace test = orangutan::tests;

namespace {

using namespace std::chrono_literals;

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
