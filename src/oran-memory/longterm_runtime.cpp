// src/oran-memory/longterm_runtime.cpp — long-term memory runtime composition.

#include <oran/memory/longterm.hpp>

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
