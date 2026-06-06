// tests/memory/test_longterm.cpp — long-term memory contract coverage.

#include <algorithm>
#include <chrono>
#include <cmath>
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

memory::longterm::VectorEmbedding make_embedding() {
  return memory::longterm::VectorEmbedding{
      .model = "test-embedding-v1",
      .values = {0.25F, -0.5F, 0.75F},
  };
}

[[nodiscard]] bool same_key(const memory::longterm::RecordKey& lhs, const memory::longterm::RecordKey& rhs) noexcept {
  return lhs.id == rhs.id && lhs.scope_key == rhs.scope_key;
}

class FakeBackend final : public memory::longterm::Backend {
public:
  explicit FakeBackend(memory::longterm::Record record)
      : FakeBackend{std::vector<memory::longterm::SearchHit>{memory::longterm::SearchHit{
            .record = record,
            .score = 0.9,
            .lexical_score = 0.8,
            .vector_score = 0.7,
        }}} {}

  explicit FakeBackend(std::vector<memory::longterm::SearchHit> hits) : FakeBackend{std::move(hits), {}} {}

  FakeBackend(std::vector<memory::longterm::SearchHit> hits, std::vector<memory::longterm::Record> records)
      : hits_{std::move(hits)}, records_{std::move(records)} {
    for (const auto& hit : hits_) {
      auto existing = std::ranges::find_if(records_, [&hit](const memory::longterm::Record& record) {
        return same_key(record.key, hit.record.key);
      });
      if (existing == records_.end()) {
        records_.push_back(hit.record);
      }
    }
  }

  [[nodiscard]] async::Awaitable<core::Result<memory::longterm::Record>> get(memory::longterm::RecordKey key) override {
    ++get_calls;
    last_key = std::move(key);
    auto record = std::ranges::find_if(records_, [this](const memory::longterm::Record& candidate) {
      return same_key(candidate.key, last_key);
    });
    if (record == records_.end()) {
      co_return std::unexpected(core::Error::not_found("record not found"));
    }
    co_return *record;
  }

  [[nodiscard]] async::Awaitable<core::Result<std::vector<memory::longterm::SearchHit>>>
  search(memory::longterm::Query query, std::size_t limit) override {
    ++search_calls;
    last_query = std::move(query);
    last_limit = limit;
    co_return hits_;
  }

  [[nodiscard]] async::Awaitable<core::Result<memory::longterm::Record>>
  upsert(memory::longterm::WriteRequest request) override {
    auto record = request.record;
    auto existing = std::ranges::find_if(records_, [&record](const memory::longterm::Record& candidate) {
      return same_key(candidate.key, record.key);
    });
    if (existing == records_.end()) {
      records_.push_back(record);
    } else {
      *existing = record;
    }
    hits_ = {memory::longterm::SearchHit{
        .record = record,
        .score = 0.9,
        .lexical_score = 0.8,
        .vector_score = std::nullopt,
    }};
    co_return record;
  }

  [[nodiscard]] async::Awaitable<core::Result<memory::longterm::Record>>
  touch(memory::longterm::TouchRequest request) override {
    ++touch_calls;
    last_touch = request;
    last_key = request.key;
    auto record = std::ranges::find_if(records_, [this](const memory::longterm::Record& candidate) {
      return same_key(candidate.key, last_key);
    });
    if (record == records_.end()) {
      co_return std::unexpected(core::Error::not_found("record not found"));
    }
    if (record->last_read_at < request.read_at) {
      record->last_read_at = request.read_at;
    }
    for (auto& hit : hits_) {
      if (same_key(hit.record.key, record->key)) {
        hit.record = *record;
      }
    }
    co_return *record;
  }

  [[nodiscard]] async::Awaitable<core::Result<memory::longterm::DecayResult>>
  decay(memory::longterm::DecayRequest request) override {
    ++decay_calls;
    last_decay = request;
    auto result = memory::longterm::DecayResult{};
    for (auto& record : records_) {
      if (record.key.scope_key != request.scope_key || record.shadow || record.last_read_at >= request.unused_before ||
          record.importance > request.importance_floor || result.shadowed_records.size() >= request.limit) {
        continue;
      }
      record.shadow = true;
      if (record.updated_at < request.decay_at) {
        record.updated_at = request.decay_at;
      }
      result.shadowed_records.push_back(record);
      for (auto& hit : hits_) {
        if (same_key(hit.record.key, record.key)) {
          hit.record = record;
        }
      }
    }
    co_return result;
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> remove(memory::longterm::RecordKey key) override {
    last_key = std::move(key);
    co_return core::Result<void>{};
  }

  memory::longterm::RecordKey last_key;
  memory::longterm::Query last_query;
  memory::longterm::TouchRequest last_touch;
  memory::longterm::DecayRequest last_decay;
  std::size_t last_limit{};
  std::size_t search_calls{};
  std::size_t get_calls{};
  std::size_t touch_calls{};
  std::size_t decay_calls{};

private:
  std::vector<memory::longterm::SearchHit> hits_;
  std::vector<memory::longterm::Record> records_;
};

class FakeVectorBackend final : public memory::longterm::VectorBackend {
public:
  FakeVectorBackend()
      : hits_{memory::longterm::VectorHit{
            .key = memory::longterm::RecordKey{.id = "rec-1", .scope_key = "agent:coder"},
            .score = 0.95,
        }} {}

  explicit FakeVectorBackend(std::vector<memory::longterm::VectorHit> hits) : hits_{std::move(hits)} {}

  [[nodiscard]] async::Awaitable<core::Result<void>> upsert(memory::longterm::VectorUpsert request) override {
    last_key = std::move(request.key);
    co_return core::Result<void>{};
  }

  [[nodiscard]] async::Awaitable<core::Result<std::vector<memory::longterm::VectorHit>>>
  search(memory::longterm::VectorSearchQuery query, std::size_t limit) override {
    ++search_calls;
    last_query = query;
    last_scope_key = std::move(query.scope_key);
    last_limit = limit;
    co_return hits_;
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> remove(memory::longterm::VectorRemoveRequest request) override {
    last_key = std::move(request.key);
    co_return core::Result<void>{};
  }

  memory::longterm::RecordKey last_key;
  memory::longterm::VectorSearchQuery last_query;
  std::string last_scope_key;
  std::size_t last_limit{};
  std::size_t search_calls{};

private:
  std::vector<memory::longterm::VectorHit> hits_;
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
  REQUIRE(memory::longterm::validate_touch_request(memory::longterm::TouchRequest{.key = record.key}).has_value());
  REQUIRE(memory::longterm::validate_decay_request(memory::longterm::DecayRequest{
                                                       .scope_key = "agent:coder",
                                                       .unused_before = core::Time{core::Time::time_point{10s}},
                                                       .importance_floor = 0.5,
                                                       .limit = 3,
                                                       .decay_at = core::Time{core::Time::time_point{20s}},
                                                   })
              .has_value());

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

  auto bad_decay = memory::longterm::validate_decay_request(memory::longterm::DecayRequest{
      .scope_key = "agent:coder",
      .unused_before = core::Time{core::Time::time_point{10s}},
      .importance_floor = 1.5,
      .limit = 1,
      .decay_at = core::Time{core::Time::time_point{20s}},
  });
  REQUIRE_FALSE(bad_decay.has_value());
  REQUIRE(bad_decay.error().kind() == core::ErrorKind::invalid_argument);

  bad_decay = memory::longterm::validate_decay_request(memory::longterm::DecayRequest{
      .scope_key = "agent:coder",
      .unused_before = core::Time{core::Time::time_point{10s}},
      .importance_floor = 0.5,
      .limit = 0,
      .decay_at = core::Time{core::Time::time_point{20s}},
  });
  REQUIRE_FALSE(bad_decay.has_value());
  REQUIRE(bad_decay.error().kind() == core::ErrorKind::invalid_argument);
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

    auto decayed = co_await backend.decay(memory::longterm::DecayRequest{
        .scope_key = "agent:coder",
        .unused_before = core::Time{core::Time::time_point{10s}},
        .importance_floor = 1.0,
        .limit = 5,
        .decay_at = core::Time{core::Time::time_point{11s}},
    });
    REQUIRE(decayed.has_value());
    REQUIRE(decayed->shadowed_records.size() == 1);
    REQUIRE(decayed->shadowed_records[0].shadow);
    REQUIRE(backend.decay_calls == 1);

    auto removed = co_await backend.remove(memory::longterm::RecordKey{.id = "rec-1", .scope_key = "agent:coder"});
    REQUIRE(removed.has_value());
  });
}

TEST_CASE("longterm::Runtime validates and delegates backend search", "[unit][memory][longterm][runtime]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    FakeBackend backend{make_record()};
    memory::longterm::Runtime runtime{backend};
    auto query = memory::longterm::Query{
        .scope_key = "agent:coder",
        .text = "workflow",
        .kinds = {memory::longterm::RecordKind::project},
    };

    auto hits = co_await runtime.search(query, 4);
    REQUIRE(hits.has_value());
    REQUIRE(hits->size() == 1);
    REQUIRE(backend.search_calls == 1);
    REQUIRE(backend.touch_calls == 0);
    REQUIRE(backend.last_query == query);
    REQUIRE(backend.last_limit == 4);
  });
}

TEST_CASE("longterm::Runtime rejects invalid recall before backend dispatch", "[unit][memory][longterm][runtime]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    FakeBackend backend{make_record()};
    memory::longterm::Runtime runtime{backend};

    auto result = co_await runtime.recall(memory::longterm::RecallRequest{
        .query =
            memory::longterm::Query{
                .scope_key = "agent:coder",
                .text = "",
                .kinds = {},
            },
        .limit = 5,
    });

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(backend.search_calls == 0);
  });
}

TEST_CASE("longterm::Runtime renders deterministic recall framing", "[unit][memory][longterm][runtime]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    auto record = make_record();
    record.body = "The repository prefers scoped slices.\nDocs move with code.";
    FakeBackend backend{record};
    memory::longterm::Runtime runtime{backend};

    auto result = co_await runtime.recall(memory::longterm::RecallRequest{
        .query =
            memory::longterm::Query{
                .scope_key = "agent:coder",
                .text = "workflow",
                .kinds = {},
            },
        .limit = 5,
    });

    REQUIRE(result.has_value());
    REQUIRE(result->hits.size() == 1);
    REQUIRE(result->framing.section_text == "Long-term memory:\n"
                                            "- [project] Build notes (id: rec-1)\n"
                                            "  The repository prefers scoped slices. Docs move with code.\n"
                                            "  tags: repo, workflow\n"
                                            "  linked: rec-0\n");
  });
}

TEST_CASE("longterm::Runtime touches recalled hits before returning", "[unit][memory][longterm][runtime]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    auto record = make_record();
    const auto previous_read_at = record.last_read_at;
    FakeBackend backend{record};
    memory::longterm::Runtime runtime{backend};

    auto result = co_await runtime.recall(memory::longterm::RecallRequest{
        .query =
            memory::longterm::Query{
                .scope_key = "agent:coder",
                .text = "workflow",
                .kinds = {},
            },
        .limit = 5,
    });

    REQUIRE(result.has_value());
    REQUIRE(result->hits.size() == 1);
    REQUIRE(backend.touch_calls == 1);
    REQUIRE(backend.last_touch.key == record.key);
    REQUIRE(backend.last_touch.read_at > previous_read_at);
    REQUIRE(result->hits[0].record.last_read_at == backend.last_touch.read_at);
  });
}

TEST_CASE("longterm recall data_json carries recalled record metadata", "[unit][memory][longterm][runtime]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    FakeBackend backend{make_record()};
    memory::longterm::Runtime runtime{backend};

    auto result = co_await runtime.recall(memory::longterm::RecallRequest{
        .query =
            memory::longterm::Query{
                .scope_key = "agent:coder",
                .text = "workflow",
                .kinds = {},
            },
        .limit = 5,
    });

    REQUIRE(result.has_value());
    auto data_json =
        memory::longterm::render_recall_data_json(std::span<const memory::longterm::SearchHit>{result->hits});
    REQUIRE(data_json.contains(R"("kind":"memory_recall")"));
    REQUIRE(data_json.contains(R"("match_count":1)"));
    REQUIRE(data_json.contains(R"("id":"rec-1")"));
    REQUIRE(data_json.contains(R"("scope_key":"agent:coder")"));
    REQUIRE(data_json.contains(R"("created_at":"1970-01-01T00:00:01.000Z")"));
    REQUIRE(data_json.contains(R"("lexical_score":0.8)"));
    REQUIRE(data_json.contains(R"("vector_score":0.7)"));
  });
}

TEST_CASE("longterm remember data_json carries saved record metadata", "[unit][memory][longterm][runtime]") {
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

TEST_CASE("longterm forget data_json carries scoped removed key", "[unit][memory][longterm][runtime]") {
  const auto key = memory::longterm::RecordKey{.id = "rec-1", .scope_key = "agent:coder"};

  const auto data_json = memory::longterm::render_forget_data_json(key);
  REQUIRE(data_json.contains(R"("kind":"memory_forget")"));
  REQUIRE(data_json.contains(R"("id":"rec-1")"));
  REQUIRE(data_json.contains(R"("scope_key":"agent:coder")"));
  REQUIRE_FALSE(data_json.contains(R"("title")"));
}

TEST_CASE("longterm::Runtime returns empty framing for empty recall", "[unit][memory][longterm][runtime]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    FakeBackend backend{std::vector<memory::longterm::SearchHit>{}};
    memory::longterm::Runtime runtime{backend};

    auto result = co_await runtime.recall(memory::longterm::RecallRequest{
        .query =
            memory::longterm::Query{
                .scope_key = "agent:coder",
                .text = "workflow",
                .kinds = {},
            },
        .limit = 5,
    });

    REQUIRE(result.has_value());
    REQUIRE(result->hits.empty());
    REQUIRE(result->framing.section_text.empty());
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

TEST_CASE("longterm::SqliteVecBackend reports disabled vector builds", "[unit][memory][longterm][sqlite-vec]") {
  TempDb db{"oran-memory-sqlite-vec-disabled"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
#if defined(ORAN_ENABLE_SQLITE_VEC)
    auto extensions = memory::longterm::SqliteVecBackend::auto_extensions();
    auto enabled_pool = storage::Pool::open(
        io.get_executor(),
        storage::PoolOptions{.path = db.string(), .reader_count = 2, .statement_cache_capacity = 16},
        extensions);
    REQUIRE(enabled_pool.has_value());
    auto& pool = *enabled_pool;
#else
    auto pool = open_pool(io, db);
#endif
    memory::longterm::SqliteVecBackend backend{pool, memory::longterm::SqliteVecBackendOptions{.dimensions = 3}};

    auto migrated = co_await backend.migrate();
#if defined(ORAN_ENABLE_SQLITE_VEC)
    REQUIRE(migrated.has_value());
#else
    REQUIRE_FALSE(migrated.has_value());
    REQUIRE(migrated.error().kind() == core::ErrorKind::config);
#endif
  });
}

#if defined(ORAN_ENABLE_SQLITE_VEC)
TEST_CASE("longterm::SqliteVecBackend upserts, searches, and removes scoped vectors",
          "[unit][memory][longterm][sqlite-vec]") {
  TempDb db{"oran-memory-sqlite-vec"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = storage::Pool::open(io.get_executor(),
                                    storage::PoolOptions{
                                        .path = db.string(),
                                        .reader_count = 2,
                                        .statement_cache_capacity = 16,
                                    },
                                    memory::longterm::SqliteVecBackend::auto_extensions());
    REQUIRE(pool.has_value());
    memory::longterm::SqliteVecBackend backend{*pool, memory::longterm::SqliteVecBackendOptions{.dimensions = 3}};

    auto migrated = co_await backend.migrate();
    REQUIRE(migrated.has_value());

    REQUIRE(
        (co_await backend.upsert(memory::longterm::VectorUpsert{
             .key = memory::longterm::RecordKey{.id = "rec-a", .scope_key = "agent:coder"},
             .embedding = memory::longterm::VectorEmbedding{.model = "test-embedding-v1", .values = {1.0F, 0.0F, 0.0F}},
         }))
            .has_value());
    REQUIRE(
        (co_await backend.upsert(memory::longterm::VectorUpsert{
             .key = memory::longterm::RecordKey{.id = "rec-b", .scope_key = "agent:coder"},
             .embedding = memory::longterm::VectorEmbedding{.model = "test-embedding-v1", .values = {0.0F, 1.0F, 0.0F}},
         }))
            .has_value());
    REQUIRE(
        (co_await backend.upsert(memory::longterm::VectorUpsert{
             .key = memory::longterm::RecordKey{.id = "rec-other", .scope_key = "agent:researcher"},
             .embedding = memory::longterm::VectorEmbedding{.model = "test-embedding-v1", .values = {1.0F, 0.0F, 0.0F}},
         }))
            .has_value());

    auto hits = co_await backend.search(
        memory::longterm::VectorSearchQuery{
            .scope_key = "agent:coder",
            .embedding = memory::longterm::VectorEmbedding{.model = "test-embedding-v1", .values = {1.0F, 0.0F, 0.0F}},
            .kinds = {},
        },
        10);
    REQUIRE(hits.has_value());
    REQUIRE(hits->size() == 2);
    REQUIRE((*hits)[0].key.id == "rec-a");
    REQUIRE((*hits)[0].key.scope_key == "agent:coder");
    REQUIRE((*hits)[0].score > (*hits)[1].score);
    REQUIRE((*hits)[1].key.id == "rec-b");

    REQUIRE((co_await backend.remove(memory::longterm::VectorRemoveRequest{
                 .key = memory::longterm::RecordKey{.id = "rec-a", .scope_key = "agent:coder"},
             }))
                .has_value());
    auto after_remove = co_await backend.search(
        memory::longterm::VectorSearchQuery{
            .scope_key = "agent:coder",
            .embedding = memory::longterm::VectorEmbedding{.model = "test-embedding-v1", .values = {1.0F, 0.0F, 0.0F}},
            .kinds = {},
        },
        10);
    REQUIRE(after_remove.has_value());
    REQUIRE(after_remove->size() == 1);
    REQUIRE((*after_remove)[0].key.id == "rec-b");
  });
}

TEST_CASE("longterm::SqliteVecBackend rejects mismatched existing vector dimensions",
          "[unit][memory][longterm][sqlite-vec]") {
  TempDb db{"oran-memory-sqlite-vec-dimensions"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = storage::Pool::open(io.get_executor(),
                                    storage::PoolOptions{
                                        .path = db.string(),
                                        .reader_count = 2,
                                        .statement_cache_capacity = 16,
                                    },
                                    memory::longterm::SqliteVecBackend::auto_extensions());
    REQUIRE(pool.has_value());
    memory::longterm::SqliteVecBackend backend{*pool, memory::longterm::SqliteVecBackendOptions{.dimensions = 3}};
    REQUIRE((co_await backend.migrate()).has_value());

    memory::longterm::SqliteVecBackend changed{*pool, memory::longterm::SqliteVecBackendOptions{.dimensions = 4}};
    auto migrated = co_await changed.migrate();
    REQUIRE_FALSE(migrated.has_value());
    REQUIRE(migrated.error().kind() == core::ErrorKind::storage);
  });
}
#endif

TEST_CASE("longterm::HybridRuntime merges lexical and vector hits deterministically",
          "[unit][memory][longterm][runtime]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    auto lexical_record = make_record("rec-lexical",
                                      "agent:coder",
                                      memory::longterm::RecordKind::project,
                                      "Lexical plan",
                                      "The lexical backend matched the project plan.");
    auto vector_record = make_record("rec-vector",
                                     "agent:coder",
                                     memory::longterm::RecordKind::project,
                                     "Vector plan",
                                     "The vector backend found a semantically close plan.");
    auto stale_record = memory::longterm::RecordKey{.id = "rec-stale", .scope_key = "agent:coder"};

    FakeBackend lexical{
        std::vector<memory::longterm::SearchHit>{memory::longterm::SearchHit{
            .record = lexical_record,
            .score = 0.5,
            .lexical_score = 0.5,
            .vector_score = std::nullopt,
        }},
        std::vector<memory::longterm::Record>{vector_record},
    };
    FakeVectorBackend vector{
        std::vector<memory::longterm::VectorHit>{
            memory::longterm::VectorHit{.key = lexical_record.key, .score = 0.75},
            memory::longterm::VectorHit{.key = vector_record.key, .score = 0.875},
            memory::longterm::VectorHit{.key = stale_record, .score = 1.0},
        },
    };
    memory::longterm::HybridRuntime runtime{lexical, vector};

    auto hits = co_await runtime.search(memory::longterm::HybridSearchRequest{
        .query =
            memory::longterm::Query{
                .scope_key = "agent:coder",
                .text = "project plan",
                .kinds = {memory::longterm::RecordKind::project},
            },
        .embedding = make_embedding(),
        .lexical_limit = 2,
        .vector_limit = 3,
        .result_limit = 2,
        .lexical_weight = 1.0,
        .vector_weight = 2.0,
    });

    REQUIRE(hits.has_value());
    REQUIRE(hits->size() == 2);
    REQUIRE((*hits)[0].record.key.id == "rec-lexical");
    REQUIRE((*hits)[0].lexical_score == 0.5);
    REQUIRE((*hits)[0].vector_score == 0.75);
    REQUIRE((*hits)[0].score == 2.0);
    REQUIRE((*hits)[1].record.key.id == "rec-vector");
    REQUIRE_FALSE((*hits)[1].lexical_score.has_value());
    REQUIRE((*hits)[1].vector_score == 0.875);
    REQUIRE((*hits)[1].score == 1.75);
    REQUIRE(lexical.search_calls == 1);
    REQUIRE(lexical.get_calls == 2);
    REQUIRE(vector.search_calls == 1);
    REQUIRE(vector.last_query.scope_key == "agent:coder");
    REQUIRE(vector.last_query.kinds ==
            std::vector<memory::longterm::RecordKind>{memory::longterm::RecordKind::project});
    REQUIRE(vector.last_limit == 3);
  });
}

TEST_CASE("longterm::HybridRuntime recalls with merged hybrid hits", "[unit][memory][longterm][runtime]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    auto record = make_record("rec-vector",
                              "agent:coder",
                              memory::longterm::RecordKind::reference,
                              "Hybrid recall",
                              "Hybrid recall can render a vector-only record.");
    FakeBackend lexical{std::vector<memory::longterm::SearchHit>{}, std::vector<memory::longterm::Record>{record}};
    FakeVectorBackend vector{std::vector<memory::longterm::VectorHit>{
        memory::longterm::VectorHit{.key = record.key, .score = 0.95},
    }};
    memory::longterm::HybridRuntime runtime{lexical, vector};

    auto recalled = co_await runtime.recall(memory::longterm::HybridSearchRequest{
        .query =
            memory::longterm::Query{
                .scope_key = "agent:coder",
                .text = "semantic recall",
                .kinds = {memory::longterm::RecordKind::reference},
            },
        .embedding = make_embedding(),
        .lexical_limit = 1,
        .vector_limit = 1,
        .result_limit = 1,
    });

    REQUIRE(recalled.has_value());
    REQUIRE(recalled->hits.size() == 1);
    REQUIRE(recalled->hits[0].record.key.id == "rec-vector");
    REQUIRE(lexical.touch_calls == 1);
    REQUIRE(lexical.last_touch.key == record.key);
    REQUIRE(recalled->hits[0].record.last_read_at == lexical.last_touch.read_at);
    REQUIRE(recalled->framing.section_text.contains("Hybrid recall"));
  });
}

TEST_CASE("longterm::HybridRuntime rejects invalid requests before backend dispatch",
          "[unit][memory][longterm][runtime]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    FakeBackend lexical{make_record()};
    FakeVectorBackend vector;
    memory::longterm::HybridRuntime runtime{lexical, vector};

    auto result = co_await runtime.search(memory::longterm::HybridSearchRequest{
        .query =
            memory::longterm::Query{
                .scope_key = "agent:coder",
                .text = "",
                .kinds = {},
            },
        .embedding = make_embedding(),
        .lexical_limit = 1,
        .vector_limit = 1,
        .result_limit = 1,
    });

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(lexical.search_calls == 0);
    REQUIRE(vector.search_calls == 0);

    auto bad_weight = memory::longterm::validate_hybrid_search_request(memory::longterm::HybridSearchRequest{
        .query =
            memory::longterm::Query{
                .scope_key = "agent:coder",
                .text = "project plan",
                .kinds = {},
            },
        .embedding = make_embedding(),
        .lexical_limit = 1,
        .vector_limit = 1,
        .result_limit = 1,
        .lexical_weight = 0.0,
        .vector_weight = 0.0,
    });
    REQUIRE_FALSE(bad_weight.has_value());
    REQUIRE(bad_weight.error().kind() == core::ErrorKind::invalid_argument);
  });
}

TEST_CASE("longterm text embeddings are deterministic and normalized", "[unit][memory][longterm][embedding]") {
  auto first = memory::longterm::make_text_embedding("Hybrid recall uses local text features.",
                                                     memory::longterm::TextEmbeddingOptions{
                                                         .model = "test-local",
                                                         .dimensions = 8,
                                                     });
  auto second = memory::longterm::make_text_embedding("hybrid RECALL uses local text features",
                                                      memory::longterm::TextEmbeddingOptions{
                                                          .model = "test-local",
                                                          .dimensions = 8,
                                                      });
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  REQUIRE(first->model == "test-local");
  REQUIRE(first->values.size() == 8);
  REQUIRE(first->values == second->values);

  auto squared_norm = 0.0F;
  for (const auto value : first->values) {
    squared_norm += value * value;
  }
  REQUIRE(std::abs(std::sqrt(squared_norm) - 1.0F) < 0.0001F);

  auto blank = memory::longterm::make_text_embedding("   ", memory::longterm::TextEmbeddingOptions{.dimensions = 8});
  REQUIRE_FALSE(blank.has_value());
  REQUIRE(blank.error().kind() == core::ErrorKind::invalid_argument);
}

TEST_CASE("longterm record embeddings include record metadata", "[unit][memory][longterm][embedding]") {
  auto record = make_record("rec-embed",
                            "agent:coder",
                            memory::longterm::RecordKind::project,
                            "Hybrid note",
                            "Record embeddings include title, body, and tags.");
  record.tags = {"vector", "recall"};
  auto embedded = memory::longterm::make_record_embedding(record,
                                                          memory::longterm::TextEmbeddingOptions{
                                                              .model = "test-local",
                                                              .dimensions = 12,
                                                          });

  REQUIRE(embedded.has_value());
  REQUIRE(embedded->model == "test-local");
  REQUIRE(embedded->values.size() == 12);
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

TEST_CASE("longterm::Fts5Backend decays stale low-importance records to shadow", "[unit][memory][longterm][fts5]") {
  TempDb db{"oran-memory-longterm-decay"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    memory::longterm::Fts5Backend backend{pool};
    auto migrated = co_await backend.migrate();
    REQUIRE(migrated.has_value());

    auto stale_low = make_record("stale-low",
                                 "agent:coder",
                                 memory::longterm::RecordKind::reference,
                                 "Stale low",
                                 "The stale visible papaya note should decay.");
    stale_low.importance = 0.2;
    stale_low.last_read_at = core::Time{core::Time::time_point{3s}};
    stale_low.updated_at = core::Time{core::Time::time_point{4s}};

    auto stale_high = make_record("stale-high",
                                  "agent:coder",
                                  memory::longterm::RecordKind::reference,
                                  "Stale high",
                                  "The high-importance papaya note should stay visible.");
    stale_high.importance = 0.9;
    stale_high.last_read_at = core::Time{core::Time::time_point{2s}};
    stale_high.updated_at = core::Time{core::Time::time_point{5s}};

    auto fresh_low = make_record("fresh-low",
                                 "agent:coder",
                                 memory::longterm::RecordKind::reference,
                                 "Fresh low",
                                 "The fresh papaya note should stay visible.");
    fresh_low.importance = 0.1;
    fresh_low.last_read_at = core::Time{core::Time::time_point{20s}};
    fresh_low.updated_at = core::Time{core::Time::time_point{21s}};

    auto other_scope = make_record("stale-other",
                                   "agent:researcher",
                                   memory::longterm::RecordKind::reference,
                                   "Other scope",
                                   "The other-scope papaya note should stay visible.");
    other_scope.importance = 0.1;
    other_scope.last_read_at = core::Time{core::Time::time_point{1s}};

    auto already_shadow = make_record("already-shadow",
                                      "agent:coder",
                                      memory::longterm::RecordKind::reference,
                                      "Already shadow",
                                      "The already-shadow papaya note should not be reported again.");
    already_shadow.importance = 0.1;
    already_shadow.last_read_at = core::Time{core::Time::time_point{1s}};
    already_shadow.shadow = true;

    REQUIRE((co_await backend.upsert(memory::longterm::WriteRequest{.record = stale_low})).has_value());
    REQUIRE((co_await backend.upsert(memory::longterm::WriteRequest{.record = stale_high})).has_value());
    REQUIRE((co_await backend.upsert(memory::longterm::WriteRequest{.record = fresh_low})).has_value());
    REQUIRE((co_await backend.upsert(memory::longterm::WriteRequest{.record = other_scope})).has_value());
    REQUIRE((co_await backend.upsert(memory::longterm::WriteRequest{.record = already_shadow})).has_value());

    const auto decay_at = core::Time{core::Time::time_point{30s}};
    auto decayed = co_await backend.decay(memory::longterm::DecayRequest{
        .scope_key = "agent:coder",
        .unused_before = core::Time{core::Time::time_point{10s}},
        .importance_floor = 0.5,
        .limit = 10,
        .decay_at = decay_at,
    });
    REQUIRE(decayed.has_value());
    REQUIRE(decayed->shadowed_records.size() == 1);
    REQUIRE(decayed->shadowed_records[0].key.id == "stale-low");
    REQUIRE(decayed->shadowed_records[0].shadow);
    REQUIRE(decayed->shadowed_records[0].updated_at == decay_at);
    REQUIRE(decayed->shadowed_records[0].last_read_at == stale_low.last_read_at);

    auto default_hits = co_await backend.search(
        memory::longterm::Query{
            .scope_key = "agent:coder",
            .text = "papaya",
            .kinds = {},
        },
        10);
    REQUIRE(default_hits.has_value());
    REQUIRE(default_hits->size() == 2);
    REQUIRE(std::ranges::none_of(*default_hits, [](const memory::longterm::SearchHit& hit) {
      return hit.record.key.id == "stale-low";
    }));

    auto including_shadow = co_await backend.search(
        memory::longterm::Query{
            .scope_key = "agent:coder",
            .text = "papaya",
            .kinds = {},
            .include_shadow = true,
        },
        10);
    REQUIRE(including_shadow.has_value());
    REQUIRE(including_shadow->size() == 4);
    auto stale_hit = std::ranges::find_if(*including_shadow, [](const memory::longterm::SearchHit& hit) {
      return hit.record.key.id == "stale-low";
    });
    REQUIRE(stale_hit != including_shadow->end());
    REQUIRE(stale_hit->record.shadow);

    auto repeated = co_await backend.decay(memory::longterm::DecayRequest{
        .scope_key = "agent:coder",
        .unused_before = core::Time{core::Time::time_point{10s}},
        .importance_floor = 0.5,
        .limit = 10,
        .decay_at = core::Time{core::Time::time_point{40s}},
    });
    REQUIRE(repeated.has_value());
    REQUIRE(repeated->shadowed_records.empty());
  });
}

TEST_CASE("longterm::Fts5Backend decay respects batch limits", "[unit][memory][longterm][fts5]") {
  TempDb db{"oran-memory-longterm-decay-limit"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    memory::longterm::Fts5Backend backend{pool};
    REQUIRE((co_await backend.migrate()).has_value());

    auto first = make_record("first",
                             "agent:coder",
                             memory::longterm::RecordKind::reference,
                             "First",
                             "The first guava record is oldest.");
    first.last_read_at = core::Time{core::Time::time_point{1s}};
    first.importance = 0.1;
    auto second = make_record("second",
                              "agent:coder",
                              memory::longterm::RecordKind::reference,
                              "Second",
                              "The second guava record is also stale.");
    second.last_read_at = core::Time{core::Time::time_point{2s}};
    second.importance = 0.1;

    REQUIRE((co_await backend.upsert(memory::longterm::WriteRequest{.record = first})).has_value());
    REQUIRE((co_await backend.upsert(memory::longterm::WriteRequest{.record = second})).has_value());

    auto decayed = co_await backend.decay(memory::longterm::DecayRequest{
        .scope_key = "agent:coder",
        .unused_before = core::Time{core::Time::time_point{10s}},
        .importance_floor = 0.5,
        .limit = 1,
        .decay_at = core::Time{core::Time::time_point{20s}},
    });
    REQUIRE(decayed.has_value());
    REQUIRE(decayed->shadowed_records.size() == 1);
    REQUIRE(decayed->shadowed_records[0].key.id == "first");

    auto default_hits = co_await backend.search(
        memory::longterm::Query{
            .scope_key = "agent:coder",
            .text = "guava",
            .kinds = {},
        },
        10);
    REQUIRE(default_hits.has_value());
    REQUIRE(default_hits->size() == 1);
    REQUIRE((*default_hits)[0].record.key.id == "second");
  });
}

TEST_CASE("longterm::Runtime recalls through Fts5Backend", "[unit][memory][longterm][runtime][fts5]") {
  TempDb db{"oran-memory-longterm-runtime-fts5"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    memory::longterm::Fts5Backend backend{pool};
    auto migrated = co_await backend.migrate();
    REQUIRE(migrated.has_value());
    memory::longterm::Runtime runtime{backend};

    auto record = make_record("runtime-rec",
                              "agent:coder",
                              memory::longterm::RecordKind::reference,
                              "Runtime recall",
                              "Runtime recall composes over the FTS5 backend.");
    record.tags = {"runtime", "fts5"};
    REQUIRE((co_await backend.upsert(memory::longterm::WriteRequest{.record = record})).has_value());

    auto result = co_await runtime.recall(memory::longterm::RecallRequest{
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
    REQUIRE(result->hits[0].record.key.id == "runtime-rec");
    REQUIRE(result->framing.section_text.contains("Runtime recall"));
    REQUIRE(result->framing.section_text.contains("tags: runtime, fts5"));
  });
}
