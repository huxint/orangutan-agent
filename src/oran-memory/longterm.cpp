// src/oran-memory/longterm.cpp — long-term memory contract validation.

#include <oran/memory/longterm.hpp>

#include <algorithm>
#include <cmath>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <oran/core/error.hpp>

namespace orangutan::memory::longterm {
namespace {

[[nodiscard]] bool is_blank(std::string_view value) noexcept {
  return std::ranges::all_of(value, [](char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r'; });
}

[[nodiscard]] bool contains_control_char(std::string_view value, bool allow_multiline) noexcept {
  return std::ranges::any_of(value, [allow_multiline](char ch) {
    if (allow_multiline && (ch == '\t' || ch == '\n' || ch == '\r')) {
      return false;
    }
    return static_cast<unsigned char>(ch) < 0x20U;
  });
}

[[nodiscard]] core::Error invalid_field(std::string field, std::string message) {
  return core::Error::invalid_argument(std::move(message)).with("field", std::move(field));
}

[[nodiscard]] core::Result<void>
validate_required(std::string_view value, std::string field, bool allow_multiline = false) {
  if (is_blank(value)) {
    return std::unexpected(invalid_field(std::move(field), "long-term memory field must not be blank"));
  }
  if (contains_control_char(value, allow_multiline)) {
    return std::unexpected(
        invalid_field(std::move(field), "long-term memory field must not contain control characters"));
  }
  return {};
}

template <typename T>
[[nodiscard]] bool has_duplicates(std::vector<T> values) {
  std::ranges::sort(values);
  return std::ranges::adjacent_find(values) != values.end();
}

[[nodiscard]] core::Result<void> validate_kinds(const std::vector<RecordKind>& kinds) {
  if (has_duplicates(kinds)) {
    return std::unexpected(invalid_field("kinds", "long-term memory query kind filters must be unique"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_string_list(const std::vector<std::string>& values, std::string field) {
  for (const auto& value : values) {
    if (auto valid = validate_required(value, field); !valid) {
      return valid;
    }
  }
  if (has_duplicates(values)) {
    return std::unexpected(invalid_field(std::move(field), "long-term memory string list values must be unique"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_limit(std::size_t limit) {
  if (limit == 0) {
    return std::unexpected(invalid_field("limit", "long-term memory search limit must be greater than zero"));
  }
  return {};
}

}  // namespace

core::Result<void> validate_key(const RecordKey& key) {
  if (auto valid = validate_required(key.id, "record_id"); !valid) {
    return valid;
  }
  if (auto valid = validate_required(key.scope_key, "scope_key"); !valid) {
    return valid;
  }
  return {};
}

core::Result<void> validate_record(const Record& record) {
  if (auto valid = validate_key(record.key); !valid) {
    return valid;
  }
  if (auto valid = validate_required(record.title, "title"); !valid) {
    return valid;
  }
  if (auto valid = validate_required(record.body, "body", true); !valid) {
    return valid;
  }
  if (!std::isfinite(record.importance) || record.importance < 0.0 || record.importance > 1.0) {
    return std::unexpected(
        invalid_field("importance", "long-term memory importance must be a finite value in the range [0, 1]"));
  }
  if (record.updated_at < record.created_at) {
    return std::unexpected(
        invalid_field("updated_at", "long-term memory updated_at must not be earlier than created_at"));
  }
  if (record.last_read_at < record.created_at) {
    return std::unexpected(
        invalid_field("last_read_at", "long-term memory last_read_at must not be earlier than created_at"));
  }
  if (auto valid = validate_string_list(record.tags, "tag"); !valid) {
    return valid;
  }
  if (auto valid = validate_string_list(record.linked_record_ids, "linked_record_id"); !valid) {
    return valid;
  }
  return {};
}

core::Result<void> validate_query(const Query& query, std::size_t limit) {
  if (auto valid = validate_required(query.scope_key, "scope_key"); !valid) {
    return valid;
  }
  if (auto valid = validate_required(query.text, "query_text", true); !valid) {
    return valid;
  }
  if (auto valid = validate_kinds(query.kinds); !valid) {
    return valid;
  }
  return validate_limit(limit);
}

core::Result<void> validate_write_request(const WriteRequest& request) {
  return validate_record(request.record);
}

core::Result<void> validate_embedding(const VectorEmbedding& embedding) {
  if (auto valid = validate_required(embedding.model, "embedding_model"); !valid) {
    return valid;
  }
  if (embedding.values.empty()) {
    return std::unexpected(
        invalid_field("embedding_values", "long-term memory embedding must contain at least one value"));
  }
  if (!std::ranges::all_of(embedding.values, [](float value) { return std::isfinite(value); })) {
    return std::unexpected(invalid_field("embedding_values", "long-term memory embedding values must be finite"));
  }
  return {};
}

core::Result<void> validate_vector_upsert(const VectorUpsert& request) {
  if (auto valid = validate_key(request.key); !valid) {
    return valid;
  }
  return validate_embedding(request.embedding);
}

core::Result<void> validate_vector_search_query(const VectorSearchQuery& query, std::size_t limit) {
  if (auto valid = validate_required(query.scope_key, "scope_key"); !valid) {
    return valid;
  }
  if (auto valid = validate_embedding(query.embedding); !valid) {
    return valid;
  }
  if (auto valid = validate_kinds(query.kinds); !valid) {
    return valid;
  }
  return validate_limit(limit);
}

core::Result<void> validate_vector_remove(const VectorRemoveRequest& request) {
  return validate_key(request.key);
}

}  // namespace orangutan::memory::longterm
