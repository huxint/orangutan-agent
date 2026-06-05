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

#include <oran/core/enum_names.hpp>

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

}  // namespace orangutan::memory::longterm
