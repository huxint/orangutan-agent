// src/oran-memory/longterm_sqlite_vec.cpp — optional sqlite-vec vector backend.

#include <oran/memory/longterm.hpp>

#include <expected>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <oran/core/error.hpp>
#include <oran/storage/pool.hpp>
#include <oran/storage/statement_cache.hpp>

#if defined(ORAN_ENABLE_SQLITE_VEC)
#define SQLITE_CORE 1
#include <sqlite-vec.h>
#endif

namespace orangutan::memory::longterm {
namespace {

#if defined(ORAN_ENABLE_SQLITE_VEC)

constexpr std::string_view kUnitSeparator{"\x1f"};
constexpr std::string_view kSchemaSql =
    "SELECT sql FROM sqlite_schema WHERE type = 'table' AND name = 'longterm_vectors'";
constexpr std::string_view kDeleteSql = "DELETE FROM longterm_vectors WHERE row_key = ?";
constexpr std::string_view kUpsertSql = "INSERT INTO longterm_vectors(row_key, scope_key, record_id, embedding) "
                                        "VALUES (?, ?, ?, vec_f32(?))";
constexpr std::string_view kSearchSql = "SELECT record_id, distance "
                                        "FROM longterm_vectors "
                                        "WHERE scope_key = ? AND embedding MATCH vec_f32(?) AND k = ? "
                                        "ORDER BY distance";

#else

[[nodiscard]] core::Error sqlite_vec_unavailable_error() {
  return core::Error::config("sqlite-vec vector memory backend is not enabled")
      .with("option", "vector_memory")
      .with("reason", "build_option_disabled");
}

#endif

#if defined(ORAN_ENABLE_SQLITE_VEC)

[[nodiscard]] std::string row_key_for(const RecordKey& key) {
  return std::format("{}{}{}", key.scope_key, kUnitSeparator, key.id);
}

[[nodiscard]] core::Result<std::int64_t> checked_limit(std::size_t limit) {
  if (limit > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
    return std::unexpected(core::Error::invalid_argument("long-term vector search limit exceeds int64"));
  }
  return static_cast<std::int64_t>(limit);
}

[[nodiscard]] std::string vector_json(std::span<const float> values) {
  std::string out;
  out.reserve(values.size() * 10U + 2U);
  out.push_back('[');
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    std::format_to(std::back_inserter(out), "{}", values[i]);
  }
  out.push_back(']');
  return out;
}

[[nodiscard]] core::Result<void> expect_done(storage::Statement& statement, std::string_view operation) {
  auto step = statement.step();
  if (!step) {
    return std::unexpected(std::move(step).error().with("operation", std::string{operation}));
  }
  if (*step != storage::StepResult::done) {
    return std::unexpected(core::Error::storage("sqlite-vec statement returned an unexpected row")
                               .with("operation", std::string{operation}));
  }
  return {};
}

[[nodiscard]] core::Error rollback_error(core::Error error, storage::Connection& connection) {
  if (auto rollback = connection.execute("ROLLBACK"); !rollback) {
    error.with("rollback_error", std::string{rollback.error().message()});
  }
  return error;
}

[[nodiscard]] core::Result<void> enable_sqlite_vec_connection(storage::Connection& connection) {
  auto version = connection.query("SELECT vec_version()");
  if (!version) {
    return std::unexpected(std::move(version).error().with("extension", "sqlite-vec"));
  }
  return {};
}

[[nodiscard]] core::Result<std::optional<std::string>> existing_schema(storage::Connection& connection) {
  auto schema = connection.query(kSchemaSql);
  if (!schema) {
    return std::unexpected(std::move(schema).error().with("table", "longterm_vectors"));
  }
  if (schema->rows.empty()) {
    return std::optional<std::string>{};
  }
  if (schema->rows.size() != 1 || schema->rows.front().values.empty() || !schema->rows.front().values.front()) {
    return std::unexpected(core::Error::storage("sqlite-vec table schema lookup returned an unexpected row")
                               .with("table", "longterm_vectors"));
  }
  return *schema->rows.front().values.front();
}

[[nodiscard]] core::Result<void> validate_existing_schema(std::string_view schema, std::size_t dimensions) {
  const auto expected_column = std::format("embedding float[{}]", dimensions);
  if (schema.find(expected_column) == std::string_view::npos) {
    return std::unexpected(core::Error::storage("sqlite-vec vector table dimension mismatch")
                               .with("table", "longterm_vectors")
                               .with("expected_dimensions", std::to_string(dimensions))
                               .with("schema", std::string{schema}));
  }
  return {};
}

[[nodiscard]] core::Result<void> bind_delete(storage::Statement& statement, const RecordKey& key) {
  return statement.bind_text(1, row_key_for(key));
}

[[nodiscard]] core::Result<void> bind_upsert(storage::Statement& statement, const VectorUpsert& request) {
  if (auto bound = statement.bind_text(1, row_key_for(request.key)); !bound) {
    return bound;
  }
  if (auto bound = statement.bind_text(2, request.key.scope_key); !bound) {
    return bound;
  }
  if (auto bound = statement.bind_text(3, request.key.id); !bound) {
    return bound;
  }
  return statement.bind_text(4, vector_json(request.embedding.values));
}

[[nodiscard]] core::Result<void>
bind_search(storage::Statement& statement, const VectorSearchQuery& query, std::int64_t limit) {
  if (auto bound = statement.bind_text(1, query.scope_key); !bound) {
    return bound;
  }
  if (auto bound = statement.bind_text(2, vector_json(query.embedding.values)); !bound) {
    return bound;
  }
  return statement.bind_int64(3, limit);
}

#endif

}  // namespace

SqliteVecBackend::SqliteVecBackend(storage::Pool& pool, SqliteVecBackendOptions options) noexcept
    : pool_{&pool}, options_{std::move(options)} {}

#if defined(ORAN_ENABLE_SQLITE_VEC)

std::vector<void (*)()> SqliteVecBackend::auto_extensions() {
  return {reinterpret_cast<void (*)()>(sqlite3_vec_init)};
}

async::Awaitable<core::Result<void>> SqliteVecBackend::migrate() {
  if (options_.dimensions == 0) {
    co_return std::unexpected(
        core::Error::invalid_argument("sqlite-vec dimensions must be greater than zero").with("field", "dimensions"));
  }

  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(std::move(writer).error());
  }
  auto& connection = writer->connection();
  if (auto enabled = enable_sqlite_vec_connection(connection); !enabled) {
    co_return std::unexpected(std::move(enabled).error());
  }

  auto schema = existing_schema(connection);
  if (!schema) {
    co_return std::unexpected(std::move(schema).error());
  }
  if (schema->has_value()) {
    if (auto valid_schema = validate_existing_schema(**schema, options_.dimensions); !valid_schema) {
      co_return std::unexpected(std::move(valid_schema).error());
    }
    co_return core::Result<void>{};
  }

  const auto create_sql = std::format(
      "CREATE VIRTUAL TABLE IF NOT EXISTS longterm_vectors USING "
      "vec0(row_key TEXT PRIMARY KEY, embedding float[{}] distance_metric=cosine, scope_key TEXT PARTITION KEY, "
      "record_id TEXT)",
      options_.dimensions);
  if (auto created = connection.execute(create_sql); !created) {
    co_return std::unexpected(std::move(created).error());
  }
  co_return core::Result<void>{};
}

async::Awaitable<core::Result<void>> SqliteVecBackend::upsert(VectorUpsert request) {
  if (auto valid = validate_vector_upsert(request); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }
  if (request.embedding.values.size() != options_.dimensions) {
    co_return std::unexpected(core::Error::invalid_argument("sqlite-vec embedding dimension mismatch")
                                  .with("expected", std::to_string(options_.dimensions))
                                  .with("actual", std::to_string(request.embedding.values.size())));
  }

  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(std::move(writer).error());
  }
  auto& connection = writer->connection();
  auto& cache = writer->statement_cache();

  if (auto begun = connection.execute("BEGIN IMMEDIATE"); !begun) {
    co_return std::unexpected(std::move(begun).error());
  }

  {
    auto cached = cache.acquire(connection, kDeleteSql);
    if (!cached) {
      co_return std::unexpected(rollback_error(std::move(cached).error(), connection));
    }
    auto& statement = cached->statement();
    if (auto bound = bind_delete(statement, request.key); !bound) {
      co_return std::unexpected(rollback_error(std::move(bound).error(), connection));
    }
    if (auto removed = expect_done(statement, "sqlite_vec_delete_before_upsert"); !removed) {
      co_return std::unexpected(rollback_error(std::move(removed).error(), connection));
    }
  }

  {
    auto cached = cache.acquire(connection, kUpsertSql);
    if (!cached) {
      co_return std::unexpected(rollback_error(std::move(cached).error(), connection));
    }
    auto& statement = cached->statement();
    if (auto bound = bind_upsert(statement, request); !bound) {
      co_return std::unexpected(rollback_error(std::move(bound).error(), connection));
    }
    if (auto inserted = expect_done(statement, "sqlite_vec_upsert"); !inserted) {
      co_return std::unexpected(rollback_error(std::move(inserted).error(), connection));
    }
  }
  if (auto committed = connection.execute("COMMIT"); !committed) {
    co_return std::unexpected(rollback_error(std::move(committed).error(), connection));
  }
  co_return core::Result<void>{};
}

async::Awaitable<core::Result<std::vector<VectorHit>>> SqliteVecBackend::search(VectorSearchQuery query,
                                                                                std::size_t limit) {
  if (auto valid = validate_vector_search_query(query, limit); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }
  if (query.embedding.values.size() != options_.dimensions) {
    co_return std::unexpected(core::Error::invalid_argument("sqlite-vec query embedding dimension mismatch")
                                  .with("expected", std::to_string(options_.dimensions))
                                  .with("actual", std::to_string(query.embedding.values.size())));
  }
  auto limit_value = checked_limit(limit);
  if (!limit_value) {
    co_return std::unexpected(std::move(limit_value).error());
  }

  auto reader = co_await pool_->acquire_reader();
  if (!reader) {
    co_return std::unexpected(std::move(reader).error());
  }
  auto cached = reader->statement_cache().acquire(reader->connection(), kSearchSql);
  if (!cached) {
    co_return std::unexpected(std::move(cached).error());
  }
  auto& statement = cached->statement();
  if (auto bound = bind_search(statement, query, *limit_value); !bound) {
    co_return std::unexpected(std::move(bound).error());
  }

  std::vector<VectorHit> hits;
  while (true) {
    auto step = statement.step();
    if (!step) {
      co_return std::unexpected(std::move(step).error());
    }
    if (*step == storage::StepResult::done) {
      break;
    }
    auto id = statement.column_text(0);
    if (!id) {
      co_return std::unexpected(std::move(id).error().with("field", "record_id"));
    }
    if (!id->has_value()) {
      co_return std::unexpected(core::Error::storage("sqlite-vec row is missing record_id").with("field", "record_id"));
    }
    auto distance = statement.column_double(1);
    if (!distance) {
      co_return std::unexpected(std::move(distance).error().with("field", "distance"));
    }
    hits.push_back(VectorHit{
        .key = RecordKey{.id = std::move(**id), .scope_key = query.scope_key},
        .score = 1.0 - *distance,
    });
  }
  co_return hits;
}

async::Awaitable<core::Result<void>> SqliteVecBackend::remove(VectorRemoveRequest request) {
  if (auto valid = validate_vector_remove(request); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }

  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(std::move(writer).error());
  }
  auto cached = writer->statement_cache().acquire(writer->connection(), kDeleteSql);
  if (!cached) {
    co_return std::unexpected(std::move(cached).error());
  }
  auto& statement = cached->statement();
  if (auto bound = bind_delete(statement, request.key); !bound) {
    co_return std::unexpected(std::move(bound).error());
  }
  if (auto removed = expect_done(statement, "sqlite_vec_remove"); !removed) {
    co_return std::unexpected(std::move(removed).error());
  }
  co_return core::Result<void>{};
}

#else

std::vector<void (*)()> SqliteVecBackend::auto_extensions() {
  return {};
}

async::Awaitable<core::Result<void>> SqliteVecBackend::migrate() {
  co_return std::unexpected(sqlite_vec_unavailable_error());
}

async::Awaitable<core::Result<void>> SqliteVecBackend::upsert(VectorUpsert request) {
  if (auto valid = validate_vector_upsert(request); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }
  co_return std::unexpected(sqlite_vec_unavailable_error());
}

async::Awaitable<core::Result<std::vector<VectorHit>>> SqliteVecBackend::search(VectorSearchQuery query,
                                                                                std::size_t limit) {
  if (auto valid = validate_vector_search_query(query, limit); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }
  co_return std::unexpected(sqlite_vec_unavailable_error());
}

async::Awaitable<core::Result<void>> SqliteVecBackend::remove(VectorRemoveRequest request) {
  if (auto valid = validate_vector_remove(request); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }
  co_return std::unexpected(sqlite_vec_unavailable_error());
}

#endif

}  // namespace orangutan::memory::longterm
