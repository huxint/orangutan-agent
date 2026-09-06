// include/oran/memory/longterm.hpp — long-term memory backend contracts.

#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
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

/// Scoped storage effects; implementations validate inputs before accessing storage.
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
  [[nodiscard]] virtual async::Awaitable<core::Result<void>> remove(RecordKey key) = 0;
};

struct Fts5BackendOptions {
  std::string migrations_directory;

  friend bool operator==(const Fts5BackendOptions&, const Fts5BackendOptions&) = default;
};

/// Scoped lexical records and their transactional FTS5 index. The pool is borrowed.
class Fts5Backend final : public Backend {
public:
  explicit Fts5Backend(storage::Pool& pool, Fts5BackendOptions options = {}) noexcept;

  [[nodiscard]] async::Awaitable<core::Result<storage::MigrationReport>> migrate();

  [[nodiscard]] async::Awaitable<core::Result<Record>> get(RecordKey key) override;
  [[nodiscard]] async::Awaitable<core::Result<std::vector<SearchHit>>> search(Query query, std::size_t limit) override;
  [[nodiscard]] async::Awaitable<core::Result<Record>> upsert(WriteRequest request) override;
  [[nodiscard]] async::Awaitable<core::Result<Record>> touch(TouchRequest request) override;
  [[nodiscard]] async::Awaitable<core::Result<void>> remove(RecordKey key) override;

private:
  storage::Pool* pool_{};
  Fts5BackendOptions options_;
};

/// Search, update read timestamps and render owned prompt framing.
/// The backend must outlive the awaited operation.
[[nodiscard]] async::Awaitable<core::Result<RecallResult>> recall(Backend& backend, RecallRequest request);

[[nodiscard]] Framing render_recall_framing(std::span<const SearchHit> hits);
[[nodiscard]] std::string render_recall_data_json(std::span<const SearchHit> hits);
[[nodiscard]] std::string render_remember_data_json(const Record& record);
[[nodiscard]] std::string render_forget_data_json(const RecordKey& key);
[[nodiscard]] core::Result<void> validate_key(const RecordKey& key);
[[nodiscard]] core::Result<void> validate_record(const Record& record);
[[nodiscard]] core::Result<void> validate_query(const Query& query, std::size_t limit);
[[nodiscard]] core::Result<void> validate_write_request(const WriteRequest& request);
[[nodiscard]] core::Result<void> validate_touch_request(const TouchRequest& request);

}  // namespace orangutan::memory::longterm

template <>
struct std::formatter<orangutan::memory::longterm::RecordKind> : std::formatter<std::string_view> {
  template <class FormatContext>
  auto format(orangutan::memory::longterm::RecordKind kind, FormatContext& ctx) const {
    return std::formatter<std::string_view>::format(orangutan::core::enum_name(kind), ctx);
  }
};
