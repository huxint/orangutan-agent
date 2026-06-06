// include/oran/memory/longterm.hpp — long-term memory backend contracts.

#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>
#include <oran/memory/framing.hpp>
#include <oran/storage/migrations.hpp>

namespace orangutan::storage {
class Pool;
}  // namespace orangutan::storage

namespace orangutan::memory::longterm {

/// Stable record kind for persistent memory rows.
///
/// Wire spelling comes from `core::enum_name(value)`; the future storage
/// repository and sqlite-vec adapter must not maintain a second string table.
enum class RecordKind : std::uint8_t {
  user,
  feedback,
  project,
  reference,
  team,
};

struct RecordKey {
  std::string id;
  std::string scope_key;

  friend bool operator==(const RecordKey&, const RecordKey&) = default;
};

struct Record {
  RecordKey key;
  RecordKind kind{RecordKind::project};
  std::string title;
  std::string body;
  core::Time created_at{core::Time::epoch()};
  core::Time updated_at{core::Time::epoch()};
  core::Time last_read_at{core::Time::epoch()};
  double importance{0.0};
  std::vector<std::string> tags;
  std::vector<std::string> linked_record_ids;
  bool shadow{false};

  friend bool operator==(const Record&, const Record&) = default;
};

struct Query {
  std::string scope_key;
  std::string text;
  std::vector<RecordKind> kinds;
  bool include_shadow{false};

  friend bool operator==(const Query&, const Query&) = default;
};

struct SearchHit {
  Record record;
  double score{0.0};
  std::optional<double> lexical_score;
  std::optional<double> vector_score;

  friend bool operator==(const SearchHit&, const SearchHit&) = default;
};

struct WriteRequest {
  Record record;

  friend bool operator==(const WriteRequest&, const WriteRequest&) = default;
};

struct TouchRequest {
  RecordKey key;
  core::Time read_at{core::Time::epoch()};

  friend bool operator==(const TouchRequest&, const TouchRequest&) = default;
};

struct DecayRequest {
  std::string scope_key;
  core::Time unused_before{core::Time::epoch()};
  double importance_floor{0.0};
  std::size_t limit{0};
  core::Time decay_at{core::Time::epoch()};

  friend bool operator==(const DecayRequest&, const DecayRequest&) = default;
};

struct DecayResult {
  std::vector<Record> shadowed_records;

  friend bool operator==(const DecayResult&, const DecayResult&) = default;
};

struct RecallRequest {
  Query query;
  std::size_t limit{0};

  friend bool operator==(const RecallRequest&, const RecallRequest&) = default;
};

struct RecallResult {
  std::vector<SearchHit> hits;
  Framing framing;

  friend bool operator==(const RecallResult&, const RecallResult&) = default;
};

struct VectorEmbedding {
  std::string model;
  std::vector<float> values;

  friend bool operator==(const VectorEmbedding&, const VectorEmbedding&) = default;
};

struct TextEmbeddingOptions {
  std::string model{"oran-local-text-v1"};
  std::size_t dimensions{64};

  friend bool operator==(const TextEmbeddingOptions&, const TextEmbeddingOptions&) = default;
};

struct VectorUpsert {
  RecordKey key;
  VectorEmbedding embedding;

  friend bool operator==(const VectorUpsert&, const VectorUpsert&) = default;
};

struct VectorSearchQuery {
  std::string scope_key;
  VectorEmbedding embedding;
  std::vector<RecordKind> kinds;
  bool include_shadow{false};

  friend bool operator==(const VectorSearchQuery&, const VectorSearchQuery&) = default;
};

struct VectorHit {
  RecordKey key;
  double score{0.0};

  friend bool operator==(const VectorHit&, const VectorHit&) = default;
};

struct VectorRemoveRequest {
  RecordKey key;

  friend bool operator==(const VectorRemoveRequest&, const VectorRemoveRequest&) = default;
};

struct HybridSearchRequest {
  Query query;
  VectorEmbedding embedding;
  std::size_t lexical_limit{0};
  std::size_t vector_limit{0};
  std::size_t result_limit{0};
  double lexical_weight{1.0};
  double vector_weight{1.0};

  friend bool operator==(const HybridSearchRequest&, const HybridSearchRequest&) = default;
};

class Backend {
public:
  Backend() = default;
  virtual ~Backend() = default;

  Backend(const Backend&) = delete;
  Backend& operator=(const Backend&) = delete;
  Backend(Backend&&) = delete;
  Backend& operator=(Backend&&) = delete;

  [[nodiscard]] virtual async::Awaitable<core::Result<Record>> get(RecordKey key) = 0;
  [[nodiscard]] virtual async::Awaitable<core::Result<std::vector<SearchHit>>> search(Query query,
                                                                                      std::size_t limit) = 0;
  [[nodiscard]] virtual async::Awaitable<core::Result<Record>> upsert(WriteRequest request) = 0;
  [[nodiscard]] virtual async::Awaitable<core::Result<Record>> touch(TouchRequest request) = 0;
  [[nodiscard]] virtual async::Awaitable<core::Result<DecayResult>> decay(DecayRequest request) = 0;
  [[nodiscard]] virtual async::Awaitable<core::Result<void>> remove(RecordKey key) = 0;
};

class VectorBackend {
public:
  VectorBackend() = default;
  virtual ~VectorBackend() = default;

  VectorBackend(const VectorBackend&) = delete;
  VectorBackend& operator=(const VectorBackend&) = delete;
  VectorBackend(VectorBackend&&) = delete;
  VectorBackend& operator=(VectorBackend&&) = delete;

  [[nodiscard]] virtual async::Awaitable<core::Result<void>> upsert(VectorUpsert request) = 0;
  [[nodiscard]] virtual async::Awaitable<core::Result<std::vector<VectorHit>>> search(VectorSearchQuery query,
                                                                                      std::size_t limit) = 0;
  [[nodiscard]] virtual async::Awaitable<core::Result<void>> remove(VectorRemoveRequest request) = 0;
};

struct Fts5BackendOptions {
  std::string migrations_directory;

  friend bool operator==(const Fts5BackendOptions&, const Fts5BackendOptions&) = default;
};

struct SqliteVecBackendOptions {
  std::size_t dimensions{0};

  friend bool operator==(const SqliteVecBackendOptions&, const SqliteVecBackendOptions&) = default;
};

/// Default lexical long-term memory backend.
///
/// `Fts5Backend` owns the built-in SQLite FTS5 schema for `memory.db` and
/// implements the lexical `Backend` contract. It remains intentionally separate
/// from `VectorBackend`; future sqlite-vec or external-vector adapters combine
/// with this backend at a runtime/search-composition layer.
class Fts5Backend final : public Backend {
public:
  explicit Fts5Backend(storage::Pool& pool, Fts5BackendOptions options = {}) noexcept;

  [[nodiscard]] async::Awaitable<core::Result<storage::MigrationReport>> migrate();

  [[nodiscard]] async::Awaitable<core::Result<Record>> get(RecordKey key) override;
  [[nodiscard]] async::Awaitable<core::Result<std::vector<SearchHit>>> search(Query query, std::size_t limit) override;
  [[nodiscard]] async::Awaitable<core::Result<Record>> upsert(WriteRequest request) override;
  [[nodiscard]] async::Awaitable<core::Result<Record>> touch(TouchRequest request) override;
  [[nodiscard]] async::Awaitable<core::Result<DecayResult>> decay(DecayRequest request) override;
  [[nodiscard]] async::Awaitable<core::Result<void>> remove(RecordKey key) override;

private:
  storage::Pool* pool_{};
  Fts5BackendOptions options_;
};

/// Optional sqlite-vec long-term vector backend.
///
/// `SqliteVecBackend` is compiled only when xmake configures
/// `--vector_memory=y`; default builds still expose the public type but return a
/// config error from migration and vector operations. The backend stores one
/// scoped vector row per `RecordKey` and satisfies the same `VectorBackend`
/// contract consumed by `HybridRuntime`.
class SqliteVecBackend final : public VectorBackend {
public:
  explicit SqliteVecBackend(storage::Pool& pool, SqliteVecBackendOptions options) noexcept;

  [[nodiscard]] static std::vector<void (*)()> auto_extensions();
  [[nodiscard]] async::Awaitable<core::Result<void>> migrate();

  [[nodiscard]] async::Awaitable<core::Result<void>> upsert(VectorUpsert request) override;
  [[nodiscard]] async::Awaitable<core::Result<std::vector<VectorHit>>> search(VectorSearchQuery query,
                                                                              std::size_t limit) override;
  [[nodiscard]] async::Awaitable<core::Result<void>> remove(VectorRemoveRequest request) override;

private:
  storage::Pool* pool_{};
  SqliteVecBackendOptions options_;
};

/// Prompt-boundary long-term memory runtime.
///
/// `Runtime` composes a lexical `Backend` into search and recall operations. It
/// does not own storage and does not query inside `agent::Loop`; callers render
/// recall once before the prompt builder consumes section-5 memory framing.
class Runtime {
public:
  explicit Runtime(Backend& backend) noexcept;

  [[nodiscard]] async::Awaitable<core::Result<std::vector<SearchHit>>> search(Query query, std::size_t limit);
  [[nodiscard]] async::Awaitable<core::Result<RecallResult>> recall(RecallRequest request);

private:
  Backend* backend_{};
};

/// Long-term memory hybrid search composition.
///
/// `HybridRuntime` is the first runtime contract for combining the default
/// lexical record store with an optional vector index. It does not own either
/// backend: vector-only hits are hydrated through the lexical `Backend::get`
/// path, stale vector rows with no record are ignored, and returned
/// `SearchHit::score` is the deterministic weighted sum of present lexical and
/// vector scores.
class HybridRuntime {
public:
  HybridRuntime(Backend& lexical_backend, VectorBackend& vector_backend) noexcept;

  [[nodiscard]] async::Awaitable<core::Result<std::vector<SearchHit>>> search(HybridSearchRequest request);
  [[nodiscard]] async::Awaitable<core::Result<RecallResult>> recall(HybridSearchRequest request);

private:
  Backend* lexical_backend_{};
  VectorBackend* vector_backend_{};
};

[[nodiscard]] Framing render_recall_framing(std::span<const SearchHit> hits);
[[nodiscard]] std::string render_recall_data_json(std::span<const SearchHit> hits);
[[nodiscard]] std::string render_remember_data_json(const Record& record);
[[nodiscard]] std::string render_forget_data_json(const RecordKey& key);
[[nodiscard]] core::Result<VectorEmbedding> make_text_embedding(std::string_view text,
                                                                TextEmbeddingOptions options = {});
[[nodiscard]] core::Result<VectorEmbedding> make_record_embedding(const Record& record,
                                                                  TextEmbeddingOptions options = {});

[[nodiscard]] core::Result<void> validate_key(const RecordKey& key);
[[nodiscard]] core::Result<void> validate_record(const Record& record);
[[nodiscard]] core::Result<void> validate_query(const Query& query, std::size_t limit);
[[nodiscard]] core::Result<void> validate_write_request(const WriteRequest& request);
[[nodiscard]] core::Result<void> validate_touch_request(const TouchRequest& request);
[[nodiscard]] core::Result<void> validate_decay_request(const DecayRequest& request);
[[nodiscard]] core::Result<void> validate_embedding(const VectorEmbedding& embedding);
[[nodiscard]] core::Result<void> validate_vector_upsert(const VectorUpsert& request);
[[nodiscard]] core::Result<void> validate_vector_search_query(const VectorSearchQuery& query, std::size_t limit);
[[nodiscard]] core::Result<void> validate_vector_remove(const VectorRemoveRequest& request);
[[nodiscard]] core::Result<void> validate_hybrid_search_request(const HybridSearchRequest& request);

}  // namespace orangutan::memory::longterm

template <>
struct std::formatter<orangutan::memory::longterm::RecordKind> : std::formatter<std::string_view> {
  template <class FormatContext>
  auto format(orangutan::memory::longterm::RecordKind kind, FormatContext& ctx) const {
    return std::formatter<std::string_view>::format(orangutan::core::enum_name(kind), ctx);
  }
};
