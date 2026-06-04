// include/oran/memory/longterm.hpp — long-term memory backend contracts.

#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>

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

struct VectorEmbedding {
  std::string model;
  std::vector<float> values;

  friend bool operator==(const VectorEmbedding&, const VectorEmbedding&) = default;
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

[[nodiscard]] core::Result<void> validate_key(const RecordKey& key);
[[nodiscard]] core::Result<void> validate_record(const Record& record);
[[nodiscard]] core::Result<void> validate_query(const Query& query, std::size_t limit);
[[nodiscard]] core::Result<void> validate_write_request(const WriteRequest& request);
[[nodiscard]] core::Result<void> validate_embedding(const VectorEmbedding& embedding);
[[nodiscard]] core::Result<void> validate_vector_upsert(const VectorUpsert& request);
[[nodiscard]] core::Result<void> validate_vector_search_query(const VectorSearchQuery& query, std::size_t limit);
[[nodiscard]] core::Result<void> validate_vector_remove(const VectorRemoveRequest& request);

}  // namespace orangutan::memory::longterm

template <>
struct std::formatter<orangutan::memory::longterm::RecordKind> : std::formatter<std::string_view> {
  template <class FormatContext>
  auto format(orangutan::memory::longterm::RecordKind kind, FormatContext& ctx) const {
    return std::formatter<std::string_view>::format(orangutan::core::enum_name(kind), ctx);
  }
};
