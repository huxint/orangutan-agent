#include "memory_tools.hpp"

#include <utility>

#include <asio/awaitable.hpp>
#include <nlohmann/json.hpp>

#include <oran/tool/builtins.hpp>

namespace orangutan::bootstrap {

async::Awaitable<core::Result<std::string>>
recall_prompt_memory(tool::Registry& registry, tool::DispatchContext& context, tool::MemoryRecallRequest request) {
  auto input = nlohmann::json{{"query", request.query}, {"limit", request.limit}};
  if (!request.kinds.empty()) {
    input["kinds"] = std::move(request.kinds);
  }
  const auto input_json = input.dump();
  auto recalled = co_await registry.dispatch(tool::kMemoryRecallName, input_json, context);
  if (!recalled) {
    co_return std::unexpected(std::move(recalled).error());
  }
  if (recalled->is_error) {
    co_return std::unexpected(core::Error::internal("prompt memory recall failed"));
  }
  co_return std::move(recalled->text);
}

}  // namespace orangutan::bootstrap
