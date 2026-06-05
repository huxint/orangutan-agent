// bench/memory/scenarios/search_fts5_vs_vector.cpp
//
// Long-term memory search A-vs-B-vs-C on one shared 10k-record corpus:
//   A. FTS5 lexical baseline      — memory::longterm::Runtime::search
//   B. brute-force cosine vectors — a bench-local VectorBackend
//   C. hybrid composition         — memory::longterm::HybridRuntime::search
//
// This is the first scenario that calls the slice-172 HybridRuntime and the
// vector half of spec 0005 AC7 (`bench/memory/search-fts5-vs-vector`). The
// cosine backend here is an in-bench reference implementation that does work
// proportional to corpus size and dimension; the gated `--vector_memory=y`
// sqlite-vec adapter will later implement the same VectorBackend contract and
// compare against these numbers.

#include <nanobench.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <numeric>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <oran/async.hpp>
#include <oran/core/time.hpp>
#include <oran/memory.hpp>
#include <oran/storage.hpp>

namespace orangutan::bench {
namespace {

constexpr std::size_t kCorpusSize = 10'000;
constexpr std::size_t kSearchLimit = 10;
constexpr std::size_t kEmbeddingDim = 256;
constexpr std::uint64_t kQuerySeed = 7;
constexpr std::string_view kScopeKey = "agent:bench";
constexpr std::string_view kEmbeddingModel = "bench-embedding-v1";

std::string make_temp_path(std::string_view tag) {
  auto path = std::filesystem::temp_directory_path() /
              (std::string{"oran-bench-"} + std::string{tag} + "-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
  return path.string();
}

class TempDb {
public:
  explicit TempDb(std::string_view tag) : path_{make_temp_path(tag)} {}

  ~TempDb() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    std::filesystem::remove(path_ + "-wal", ec);
    std::filesystem::remove(path_ + "-shm", ec);
  }

  TempDb(const TempDb&) = delete;
  TempDb& operator=(const TempDb&) = delete;

  [[nodiscard]] const std::string& path() const noexcept {
    return path_;
  }

private:
  std::string path_;
};

[[nodiscard]] memory::longterm::RecordKind kind_for(std::size_t index) noexcept {
  switch (index % 4U) {
    case 0:
      return memory::longterm::RecordKind::project;
    case 1:
      return memory::longterm::RecordKind::reference;
    case 2:
      return memory::longterm::RecordKind::user;
    default:
      return memory::longterm::RecordKind::feedback;
  }
}

[[nodiscard]] std::string record_id(std::size_t index) {
  return std::format("rec-{:05}", index);
}

[[nodiscard]] memory::longterm::RecordKey record_key(std::size_t index) {
  return memory::longterm::RecordKey{.id = record_id(index), .scope_key = std::string{kScopeKey}};
}

[[nodiscard]] memory::longterm::Record make_record(std::size_t index) {
  const auto created = core::Time{core::Time::time_point{std::chrono::seconds{1}}};
  const auto updated = core::Time{core::Time::time_point{std::chrono::seconds{2}}};
  return memory::longterm::Record{
      .key = record_key(index),
      .kind = kind_for(index),
      .title = std::format("React agent loop note {}", index),
      .body = std::format("Synthetic long-term memory row {} for the react agent loop search comparison. "
                          "The record mentions scoped tools, recall ranking, and prompt-boundary memory.",
                          index),
      .created_at = created,
      .updated_at = updated,
      .last_read_at = updated,
      .importance = static_cast<double>(index % 100U) / 100.0,
      .tags = {"react", "agent", "loop", std::format("bucket-{}", index % 16U)},
      .linked_record_ids = {},
  };
}

// Deterministic L2-normalized embedding seeded by `seed`. Bench-only: the
// values are not semantically meaningful. They exist so the cosine backend does
// floating-point work proportional to the corpus size and dimension, and so the
// query reproduces the same ranking on every run.
[[nodiscard]] std::vector<float> make_unit_embedding(std::uint64_t seed) {
  std::mt19937_64 rng{seed};
  std::uniform_real_distribution<float> dist{-1.0F, 1.0F};
  std::vector<float> values(kEmbeddingDim);
  std::ranges::generate(values, [&] { return dist(rng); });
  const auto norm = std::sqrt(std::transform_reduce(values.begin(), values.end(), values.begin(), 0.0F));
  if (norm > 0.0F) {
    for (auto& value : values) {
      value /= norm;
    }
  }
  return values;
}

[[nodiscard]] memory::longterm::VectorEmbedding make_query_embedding() {
  return memory::longterm::VectorEmbedding{
      .model = std::string{kEmbeddingModel},
      .values = make_unit_embedding(kQuerySeed),
  };
}

[[nodiscard]] memory::longterm::Query make_lexical_query() {
  return memory::longterm::Query{
      .scope_key = std::string{kScopeKey},
      .text = "react agent loop",
      .kinds = {},
  };
}

// In-bench reference VectorBackend: a linear cosine scan over normalized
// embeddings filtered by scope. Kind/shadow filtering is intentionally left to
// HybridRuntime (which re-checks the hydrated record), matching the contract a
// real sqlite-vec adapter satisfies — VectorUpsert carries no kind/shadow.
class BruteForceVectorBackend final : public memory::longterm::VectorBackend {
public:
  [[nodiscard]] async::Awaitable<core::Result<void>> upsert(memory::longterm::VectorUpsert request) override {
    auto values = request.embedding.values;
    const auto norm = std::sqrt(std::transform_reduce(values.begin(), values.end(), values.begin(), 0.0F));
    if (norm > 0.0F) {
      for (auto& value : values) {
        value /= norm;
      }
    }
    auto existing = std::ranges::find_if(entries_, [&request](const Entry& entry) { return entry.key == request.key; });
    if (existing == entries_.end()) {
      entries_.push_back(Entry{.key = std::move(request.key), .unit = std::move(values)});
    } else {
      existing->unit = std::move(values);
    }
    co_return core::Result<void>{};
  }

  [[nodiscard]] async::Awaitable<core::Result<std::vector<memory::longterm::VectorHit>>>
  search(memory::longterm::VectorSearchQuery query, std::size_t limit) override {
    std::vector<memory::longterm::VectorHit> hits;
    hits.reserve(entries_.size());
    for (const auto& entry : entries_) {
      if (entry.key.scope_key != query.scope_key) {
        continue;
      }
      const auto score =
          std::transform_reduce(entry.unit.begin(), entry.unit.end(), query.embedding.values.begin(), 0.0F);
      hits.push_back(memory::longterm::VectorHit{.key = entry.key, .score = static_cast<double>(score)});
    }
    const auto keep = std::min(limit, hits.size());
    std::ranges::partial_sort(hits,
                              hits.begin() + static_cast<std::ptrdiff_t>(keep),
                              [](const memory::longterm::VectorHit& lhs, const memory::longterm::VectorHit& rhs) {
                                return lhs.score > rhs.score;
                              });
    hits.resize(keep);
    co_return hits;
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> remove(memory::longterm::VectorRemoveRequest request) override {
    std::erase_if(entries_, [&request](const Entry& entry) { return entry.key == request.key; });
    co_return core::Result<void>{};
  }

private:
  struct Entry {
    memory::longterm::RecordKey key;
    std::vector<float> unit;
  };

  std::vector<Entry> entries_;
};

void seed_corpus(asio::io_context& io, memory::longterm::Fts5Backend& fts5, BruteForceVectorBackend& vectors) {
  bool seeded = false;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        auto migrated = co_await fts5.migrate();
        if (!migrated) {
          std::abort();
        }
        for (std::size_t i = 0; i < kCorpusSize; ++i) {
          auto stored = co_await fts5.upsert(memory::longterm::WriteRequest{.record = make_record(i)});
          if (!stored) {
            std::abort();
          }
          auto indexed = co_await vectors.upsert(memory::longterm::VectorUpsert{
              .key = record_key(i),
              .embedding =
                  memory::longterm::VectorEmbedding{
                      .model = std::string{kEmbeddingModel},
                      .values = make_unit_embedding(i + 1),
                  },
          });
          if (!indexed) {
            std::abort();
          }
        }
        seeded = true;
        co_return;
      },
      asio::detached);
  io.restart();
  io.run();
  if (!seeded) {
    std::abort();
  }
}

[[gnu::noinline]] std::size_t run_fts5_search(asio::io_context& io, memory::longterm::Runtime& runtime) {
  std::size_t rows = 0;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        auto hits = co_await runtime.search(make_lexical_query(), kSearchLimit);
        if (!hits) {
          std::abort();
        }
        rows = hits->size();
        co_return;
      },
      asio::detached);
  io.restart();
  io.run();
  if (rows != kSearchLimit) {
    std::abort();
  }
  return rows;
}

[[gnu::noinline]] std::size_t run_vector_search(asio::io_context& io,
                                                BruteForceVectorBackend& vectors,
                                                const memory::longterm::VectorEmbedding& query) {
  std::size_t rows = 0;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        auto hits = co_await vectors.search(
            memory::longterm::VectorSearchQuery{
                .scope_key = std::string{kScopeKey},
                .embedding = query,
                .kinds = {},
            },
            kSearchLimit);
        if (!hits) {
          std::abort();
        }
        rows = hits->size();
        co_return;
      },
      asio::detached);
  io.restart();
  io.run();
  if (rows != kSearchLimit) {
    std::abort();
  }
  return rows;
}

[[gnu::noinline]] std::size_t run_hybrid_search(asio::io_context& io,
                                                memory::longterm::HybridRuntime& runtime,
                                                const memory::longterm::VectorEmbedding& query) {
  std::size_t rows = 0;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        auto hits = co_await runtime.search(memory::longterm::HybridSearchRequest{
            .query = make_lexical_query(),
            .embedding = query,
            .lexical_limit = kSearchLimit,
            .vector_limit = kSearchLimit,
            .result_limit = kSearchLimit,
            .lexical_weight = 1.0,
            .vector_weight = 1.0,
        });
        if (!hits) {
          std::abort();
        }
        rows = hits->size();
        co_return;
      },
      asio::detached);
  io.restart();
  io.run();
  if (rows != kSearchLimit) {
    std::abort();
  }
  return rows;
}

}  // namespace

void register_search_fts5_vs_vector(ankerl::nanobench::Bench& bench) {
  TempDb db{"memory-longterm-search"};

  asio::io_context io;
  auto pool =
      storage::Pool::open(io.get_executor(),
                          storage::PoolOptions{.path = db.path(), .reader_count = 2, .statement_cache_capacity = 32});
  if (!pool) {
    std::abort();
  }
  memory::longterm::Fts5Backend fts5{*pool};
  BruteForceVectorBackend vectors;
  seed_corpus(io, fts5, vectors);

  memory::longterm::Runtime lexical_runtime{fts5};
  memory::longterm::HybridRuntime hybrid_runtime{fts5, vectors};
  const auto query_embedding = make_query_embedding();

  // Search is far heavier than the session-store batch, so scale epochs down to
  // keep the scenario practical (mirrors scenarios/longterm_fts5.cpp).
  bench.epochs(5).minEpochIterations(20).warmup(2);

  bench.run("memory.longterm_search_fts5_only_10k_limit10",
            [&] { ankerl::nanobench::doNotOptimizeAway(run_fts5_search(io, lexical_runtime)); });
  bench.run("memory.longterm_search_vector_cosine_10k_limit10",
            [&] { ankerl::nanobench::doNotOptimizeAway(run_vector_search(io, vectors, query_embedding)); });
  bench.run("memory.longterm_search_hybrid_fts5_vector_10k_limit10",
            [&] { ankerl::nanobench::doNotOptimizeAway(run_hybrid_search(io, hybrid_runtime, query_embedding)); });
}

}  // namespace orangutan::bench
