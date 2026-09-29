#include "commands.hpp"

#include <algorithm>
#include <array>
#include <format>

#include <asio/co_spawn.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <oran/bootstrap/runtime_assembly.hpp>
#include <oran/memory/session.hpp>
#include <oran/provider/system.hpp>
#include <oran/storage/trace_repository.hpp>

namespace orangutan::telegram_host {
namespace {
constexpr std::array catalogue{
    std::pair{"new", "开启新会话，保留旧记录和长期记忆"},
    std::pair{"status", "查看当前会话、模型和最近用量"},
    std::pair{"help", "查看命令和使用说明"},
    std::pair{"whoami", "查看自己的 Telegram 用户和聊天 ID"},
};

std::string lowercase(std::string_view text) {
  std::string value{text};
  std::ranges::transform(value, value.begin(), [](char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
  });
  return value;
}
}  // namespace

std::optional<Command> parse_command(std::string_view text, std::string_view bot_username) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos || text[first] != '/')
    return std::nullopt;
  text.remove_prefix(first + 1);
  const auto end = text.find_first_of(" \t\r\n");
  auto token = text.substr(0, end);
  auto target = token.find('@');
  const auto name = token.substr(0, target);
  if (name.empty() || name.size() > 32 || !std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
      }))
    return std::nullopt;
  return Command{lowercase(name),
                 end != std::string_view::npos && text.find_first_not_of(" \t\r\n", end) != std::string_view::npos,
                 target == std::string_view::npos || lowercase(token.substr(target + 1)) == lowercase(bot_username)};
}

Json command_menu(std::string_view user) {
  auto commands = Json::array();
  for (const auto& [name, description] : catalogue)
    commands.push_back({{"command", name}, {"description", description}});
  return {{"scope", {{"type", "chat"}, {"chat_id", user}}}, {"commands", std::move(commands)}};
}

std::string command_help() {
  std::string text = "可以直接发送文字、图片，或回复并引用已有消息。\n\n";
  for (const auto& [name, description] : catalogue)
    text += std::format("/{} — {}\n", name, description);
  text += "\n/start 和 /commands 也可查看帮助。命令单独发送，不需要调用模型。";
  return text;
}

async::Awaitable<core::Result<std::string>> session_status(bootstrap::RuntimeAssembly& assembly,
                                                           const provider::Route& route,
                                                           core::TurnId session,
                                                           asio::any_io_executor worker) {
  const auto cancellation = co_await asio::this_coro::cancellation_state;
  if (cancellation.cancelled() != asio::cancellation_type::none)
    co_return std::unexpected(core::Error::cancelled());
  std::string text = std::format("配置模型：`{}`\n长期记忆：{}\n",
                                 route.primary.model,
                                 assembly.longterm_memory_enabled() ? "开启" : "关闭");
  if (auto* store = assembly.session_store()) {
    auto snapshot = co_await asio::co_spawn(worker,
                                            store->load_context({core::format_turn_id_hex(session)}, {"telegram"}),
                                            asio::use_awaitable);
    if (!snapshot)
      co_return std::unexpected(snapshot.error());
    text += std::format("已保存消息：{} 条\n摘要已覆盖：{} 条\n",
                        snapshot->message_count,
                        snapshot->checkpoint.covered_sequence);
  } else {
    text += "会话存储：关闭\n";
  }
  if (auto* trace = assembly.trace_repository()) {
    auto turns =
        co_await asio::co_spawn(worker,
                                trace->list_turns({.session_id = session, .agent_key = "telegram", .limit = 1}),
                                asio::use_awaitable);
    if (!turns)
      co_return std::unexpected(turns.error());
    if (turns->empty()) {
      text += "最近调用：暂无记录";
    } else {
      const auto& last = turns->front();
      text += std::format("最近调用模型：`{}`\n最近调用 token：输入 {} / 输出 {}\n缓存读取：{} token",
                          last.route_model,
                          last.input_tokens,
                          last.output_tokens,
                          last.cache_read_tokens);
    }
  } else {
    text += "最近调用：追踪已关闭";
  }
  co_return text;
}
}  // namespace orangutan::telegram_host
