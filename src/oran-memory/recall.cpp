#include <oran/memory/longterm.hpp>

#include <algorithm>
#include <utility>

#include <oran/core/error.hpp>
#include <oran/core/time.hpp>

namespace orangutan::memory::longterm {

async::Awaitable<core::Result<RecallResult>> recall(Backend& backend, RecallRequest request) {
  if (auto valid = validate_recall_request(request); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }

  std::vector<SearchHit> hits;
  if (!request.record_id.empty()) {
    auto record =
        co_await backend.get(RecordKey{.id = std::move(request.record_id), .scope_key = request.query.scope_key});
    if (!record) {
      co_return std::unexpected(std::move(record).error());
    }
    if ((!request.query.include_shadow && record->shadow) ||
        (!request.query.kinds.empty() && !std::ranges::contains(request.query.kinds, record->kind))) {
      co_return std::unexpected(core::Error::not_found("long-term memory record not found"));
    }
    hits.push_back(SearchHit{.record = std::move(*record)});
  } else {
    auto found = co_await backend.search(std::move(request.query), request.limit);
    if (!found) {
      co_return std::unexpected(std::move(found).error());
    }
    hits = std::move(*found);
  }

  const auto read_at = core::time::now_utc();
  for (auto& hit : hits) {
    auto touched = co_await backend.touch(TouchRequest{.key = hit.record.key, .read_at = read_at});
    if (!touched) {
      co_return std::unexpected(std::move(touched).error());
    }
    // Keep the selected content snapshot if another writer changed this note
    // while the read timestamp was being recorded.
    hit.record.last_read_at = touched->last_read_at;
  }

  auto framing = render_recall_framing(hits);
  co_return RecallResult{
      .hits = std::move(hits),
      .framing = std::move(framing),
  };
}

async::Awaitable<core::Result<IndexResult>> index(Backend& backend, IndexRequest request) {
  if (auto valid = validate_index_request(request); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }
  auto entries = co_await backend.list(request);
  if (!entries) {
    co_return std::unexpected(std::move(entries).error());
  }
  co_return make_index(*entries, request);
}

}  // namespace orangutan::memory::longterm
