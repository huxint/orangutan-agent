// src/oran-memory/longterm_runtime.cpp — long-term memory runtime composition.

#include <oran/memory/longterm.hpp>

#include <algorithm>
#include <expected>
#include <format>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/core/time.hpp>

namespace orangutan::memory::longterm {
namespace {

[[nodiscard]] bool is_space(char ch) noexcept {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

[[nodiscard]] std::string flatten_for_prompt(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool pending_space = false;
  for (const auto ch : text) {
    if (is_space(ch)) {
      pending_space = !out.empty();
      continue;
    }
    if (pending_space) {
      out.push_back(' ');
      pending_space = false;
    }
    out.push_back(ch);
  }
  return out;
}

void append_string_list(std::string& out, std::string_view label, std::span<const std::string> values) {
  if (values.empty()) {
    return;
  }
  std::format_to(std::back_inserter(out), "  {}: ", label);
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      std::format_to(std::back_inserter(out), ", ");
    }
    std::format_to(std::back_inserter(out), "{}", values[i]);
  }
  std::format_to(std::back_inserter(out), "\n");
}

[[nodiscard]] nlohmann::json optional_double_json(std::optional<double> value) {
  if (!value.has_value()) {
    return nullptr;
  }
  return *value;
}

[[nodiscard]] nlohmann::json string_list_json(std::span<const std::string> values) {
  auto out = nlohmann::json::array();
  for (const auto& value : values) {
    out.push_back(value);
  }
  return out;
}

[[nodiscard]] nlohmann::json record_json(const Record& record) {
  return nlohmann::json{
      {"id", record.key.id},
      {"scope_key", record.key.scope_key},
      {"kind", std::string{core::enum_name(record.kind)}},
      {"title", record.title},
      {"body", record.body},
      {"created_at", core::time::format_iso8601_utc(record.created_at)},
      {"updated_at", core::time::format_iso8601_utc(record.updated_at)},
      {"last_read_at", core::time::format_iso8601_utc(record.last_read_at)},
      {"importance", record.importance},
      {"tags", string_list_json(record.tags)},
      {"linked_record_ids", string_list_json(record.linked_record_ids)},
      {"shadow", record.shadow},
  };
}

[[nodiscard]] nlohmann::json record_json(const SearchHit& hit) {
  auto out = record_json(hit.record);
  out["score"] = hit.score;
  out["lexical_score"] = optional_double_json(hit.lexical_score);
  out["vector_score"] = optional_double_json(hit.vector_score);
  return out;
}

[[nodiscard]] bool same_key(const RecordKey& lhs, const RecordKey& rhs) noexcept {
  return lhs.id == rhs.id && lhs.scope_key == rhs.scope_key;
}

[[nodiscard]] bool record_matches_query(const Record& record, const Query& query) {
  if (record.key.scope_key != query.scope_key) {
    return false;
  }
  if (record.shadow && !query.include_shadow) {
    return false;
  }
  return query.kinds.empty() || std::ranges::contains(query.kinds, record.kind);
}

void recompute_hybrid_score(SearchHit& hit, double lexical_weight, double vector_weight) noexcept {
  auto score = 0.0;
  if (hit.lexical_score.has_value()) {
    score += lexical_weight * *hit.lexical_score;
  }
  if (hit.vector_score.has_value()) {
    score += vector_weight * *hit.vector_score;
  }
  hit.score = score;
}

void sort_and_cap_hits(std::vector<SearchHit>& hits, std::size_t limit) {
  std::ranges::sort(hits, [](const SearchHit& lhs, const SearchHit& rhs) {
    if (lhs.score != rhs.score) {
      return lhs.score > rhs.score;
    }
    if (lhs.record.updated_at != rhs.record.updated_at) {
      return lhs.record.updated_at > rhs.record.updated_at;
    }
    return lhs.record.key.id < rhs.record.key.id;
  });
  if (hits.size() > limit) {
    hits.resize(limit);
  }
}

[[nodiscard]] async::Awaitable<core::Result<void>> touch_recalled_hits(Backend& backend, std::vector<SearchHit>& hits) {
  if (hits.empty()) {
    co_return core::Result<void>{};
  }

  const auto read_at = core::time::now_utc();
  for (auto& hit : hits) {
    auto touched = co_await backend.touch(TouchRequest{.key = hit.record.key, .read_at = read_at});
    if (!touched) {
      co_return std::unexpected(std::move(touched).error());
    }
    hit.record = std::move(*touched);
  }
  co_return core::Result<void>{};
}

}  // namespace

Runtime::Runtime(Backend& backend) noexcept : backend_{&backend} {}

async::Awaitable<core::Result<std::vector<SearchHit>>> Runtime::search(Query query, std::size_t limit) {
  if (auto valid = validate_query(query, limit); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }
  auto hits = co_await backend_->search(std::move(query), limit);
  if (!hits) {
    co_return std::unexpected(std::move(hits).error());
  }
  co_return std::move(*hits);
}

async::Awaitable<core::Result<RecallResult>> Runtime::recall(RecallRequest request) {
  auto hits = co_await search(std::move(request.query), request.limit);
  if (!hits) {
    co_return std::unexpected(std::move(hits).error());
  }
  if (auto touched = co_await touch_recalled_hits(*backend_, *hits); !touched) {
    co_return std::unexpected(std::move(touched).error());
  }
  auto framing = render_recall_framing(*hits);
  co_return RecallResult{
      .hits = std::move(*hits),
      .framing = std::move(framing),
  };
}

HybridRuntime::HybridRuntime(Backend& lexical_backend, VectorBackend& vector_backend) noexcept
    : lexical_backend_{&lexical_backend}, vector_backend_{&vector_backend} {}

async::Awaitable<core::Result<std::vector<SearchHit>>> HybridRuntime::search(HybridSearchRequest request) {
  if (auto valid = validate_hybrid_search_request(request); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }

  auto lexical_hits = co_await lexical_backend_->search(request.query, request.lexical_limit);
  if (!lexical_hits) {
    co_return std::unexpected(std::move(lexical_hits).error());
  }

  auto vector_hits = co_await vector_backend_->search(
      VectorSearchQuery{
          .scope_key = request.query.scope_key,
          .embedding = request.embedding,
          .kinds = request.query.kinds,
          .include_shadow = request.query.include_shadow,
      },
      request.vector_limit);
  if (!vector_hits) {
    co_return std::unexpected(std::move(vector_hits).error());
  }

  std::vector<SearchHit> combined;
  combined.reserve(lexical_hits->size() + vector_hits->size());
  for (auto hit : *lexical_hits) {
    if (!hit.lexical_score.has_value()) {
      hit.lexical_score = hit.score;
    }
    hit.vector_score = std::nullopt;
    recompute_hybrid_score(hit, request.lexical_weight, request.vector_weight);
    combined.push_back(std::move(hit));
  }

  for (const auto& vector_hit : *vector_hits) {
    auto existing = std::ranges::find_if(combined, [&vector_hit](const SearchHit& hit) {
      return same_key(hit.record.key, vector_hit.key);
    });
    if (existing != combined.end()) {
      existing->vector_score = vector_hit.score;
      recompute_hybrid_score(*existing, request.lexical_weight, request.vector_weight);
      continue;
    }

    auto record = co_await lexical_backend_->get(vector_hit.key);
    if (!record) {
      if (record.error().kind() == core::ErrorKind::not_found) {
        continue;
      }
      co_return std::unexpected(std::move(record).error());
    }
    if (!record_matches_query(*record, request.query)) {
      continue;
    }

    auto hit = SearchHit{
        .record = std::move(*record),
        .score = 0.0,
        .lexical_score = std::nullopt,
        .vector_score = vector_hit.score,
    };
    recompute_hybrid_score(hit, request.lexical_weight, request.vector_weight);
    combined.push_back(std::move(hit));
  }

  sort_and_cap_hits(combined, request.result_limit);
  co_return combined;
}

async::Awaitable<core::Result<RecallResult>> HybridRuntime::recall(HybridSearchRequest request) {
  auto hits = co_await search(std::move(request));
  if (!hits) {
    co_return std::unexpected(std::move(hits).error());
  }
  if (auto touched = co_await touch_recalled_hits(*lexical_backend_, *hits); !touched) {
    co_return std::unexpected(std::move(touched).error());
  }
  auto framing = render_recall_framing(*hits);
  co_return RecallResult{
      .hits = std::move(*hits),
      .framing = std::move(framing),
  };
}

Framing render_recall_framing(std::span<const SearchHit> hits) {
  if (hits.empty()) {
    return Framing{};
  }

  std::string text;
  std::format_to(std::back_inserter(text), "Long-term memory:\n");
  for (const auto& hit : hits) {
    const auto& record = hit.record;
    std::format_to(std::back_inserter(text),
                   "- [{}{}] {} (id: {})\n",
                   core::enum_name(record.kind),
                   record.shadow ? " shadow" : "",
                   record.title,
                   record.key.id);
    std::format_to(std::back_inserter(text), "  {}\n", flatten_for_prompt(record.body));
    append_string_list(text, "tags", record.tags);
    append_string_list(text, "linked", record.linked_record_ids);
  }
  return Framing{.section_text = std::move(text)};
}

std::string render_recall_data_json(std::span<const SearchHit> hits) {
  auto records = nlohmann::json::array();
  for (const auto& hit : hits) {
    records.push_back(record_json(hit));
  }
  return nlohmann::json{
      {"kind", "memory_recall"},
      {"match_count", hits.size()},
      {"records", std::move(records)},
  }
      .dump();
}

std::string render_remember_data_json(const Record& record) {
  return nlohmann::json{
      {"kind", "memory_remember"},
      {"record", record_json(record)},
  }
      .dump();
}

std::string render_forget_data_json(const RecordKey& key) {
  return nlohmann::json{
      {"kind", "memory_forget"},
      {"record",
       nlohmann::json{
           {"id", key.id},
           {"scope_key", key.scope_key},
       }},
  }
      .dump();
}

}  // namespace orangutan::memory::longterm
