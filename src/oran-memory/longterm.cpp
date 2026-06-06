// src/oran-memory/longterm.cpp — long-term memory contract validation.

#include <oran/memory/longterm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
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

[[nodiscard]] core::Result<void> validate_weight(double weight, std::string field) {
  if (!std::isfinite(weight) || weight < 0.0) {
    return std::unexpected(
        invalid_field(std::move(field), "long-term memory hybrid search weights must be finite and non-negative"));
  }
  return {};
}

[[nodiscard]] unsigned char normalize_embedding_char(char ch) noexcept {
  auto byte = static_cast<unsigned char>(ch);
  if (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z')) {
    byte = static_cast<unsigned char>(byte - static_cast<unsigned char>('A') + static_cast<unsigned char>('a'));
  }
  return byte;
}

[[nodiscard]] bool is_embedding_separator(char ch) noexcept {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == ',' || ch == ';' || ch == ':' || ch == '.' ||
         ch == '/' || ch == '\\' || ch == '(' || ch == ')' || ch == '[' || ch == ']' || ch == '{' || ch == '}' ||
         ch == '"' || ch == '\'';
}

[[nodiscard]] std::uint64_t fnv_mix(std::uint64_t hash, unsigned char byte) noexcept {
  constexpr auto kPrime = std::uint64_t{1099511628211ULL};
  hash ^= static_cast<std::uint64_t>(byte);
  hash *= kPrime;
  return hash;
}

void add_embedding_feature(std::vector<float>& values, std::uint64_t hash) {
  const auto index = static_cast<std::size_t>(hash % values.size());
  const auto sign = (hash & (std::uint64_t{1} << 63U)) == 0 ? 1.0F : -1.0F;
  values[index] += sign;
}

[[nodiscard]] core::Result<void> validate_text_embedding_options(const TextEmbeddingOptions& options) {
  if (auto valid = validate_required(options.model, "embedding_model"); !valid) {
    return valid;
  }
  if (options.dimensions == 0) {
    return std::unexpected(
        invalid_field("embedding_dimensions", "long-term memory embedding dimensions must be positive"));
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

core::Result<void> validate_touch_request(const TouchRequest& request) {
  return validate_key(request.key);
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

core::Result<void> validate_hybrid_search_request(const HybridSearchRequest& request) {
  if (auto valid = validate_query(request.query, request.lexical_limit); !valid) {
    return valid;
  }
  if (auto valid = validate_embedding(request.embedding); !valid) {
    return valid;
  }
  if (auto valid = validate_limit(request.vector_limit); !valid) {
    return valid;
  }
  if (auto valid = validate_limit(request.result_limit); !valid) {
    return valid;
  }
  if (auto valid = validate_weight(request.lexical_weight, "lexical_weight"); !valid) {
    return valid;
  }
  if (auto valid = validate_weight(request.vector_weight, "vector_weight"); !valid) {
    return valid;
  }
  if (request.lexical_weight == 0.0 && request.vector_weight == 0.0) {
    return std::unexpected(invalid_field("weights", "long-term memory hybrid search requires a non-zero weight"));
  }
  return {};
}

core::Result<VectorEmbedding> make_text_embedding(std::string_view text, TextEmbeddingOptions options) {
  if (auto valid = validate_text_embedding_options(options); !valid) {
    return std::unexpected(std::move(valid).error());
  }
  if (auto valid = validate_required(text, "embedding_text", true); !valid) {
    return std::unexpected(std::move(valid).error());
  }

  constexpr auto kOffset = std::uint64_t{14695981039346656037ULL};
  auto values = std::vector<float>(options.dimensions, 0.0F);
  auto token_hash = kOffset;
  auto in_token = false;
  for (const auto ch : text) {
    if (is_embedding_separator(ch)) {
      if (in_token) {
        add_embedding_feature(values, token_hash);
        token_hash = kOffset;
        in_token = false;
      }
      continue;
    }
    token_hash = fnv_mix(token_hash, normalize_embedding_char(ch));
    in_token = true;
  }
  if (in_token) {
    add_embedding_feature(values, token_hash);
  }

  auto squared_norm = 0.0F;
  for (const auto value : values) {
    squared_norm += value * value;
  }
  if (squared_norm == 0.0F) {
    return std::unexpected(invalid_field("embedding_text", "long-term memory embedding text produced no features"));
  }
  const auto norm = std::sqrt(squared_norm);
  for (auto& value : values) {
    value /= norm;
  }
  return VectorEmbedding{
      .model = std::move(options.model),
      .values = std::move(values),
  };
}

core::Result<VectorEmbedding> make_record_embedding(const Record& record, TextEmbeddingOptions options) {
  if (auto valid = validate_record(record); !valid) {
    return std::unexpected(std::move(valid).error());
  }

  auto text = std::string{};
  text.reserve(record.title.size() + record.body.size() + 32U);
  text.append(record.title);
  text.push_back('\n');
  text.append(record.body);
  for (const auto& tag : record.tags) {
    text.push_back('\n');
    text.append(tag);
  }
  for (const auto& id : record.linked_record_ids) {
    text.push_back('\n');
    text.append(id);
  }
  return make_text_embedding(text, std::move(options));
}

}  // namespace orangutan::memory::longterm
