#include "host.hpp"

#include <algorithm>
#include <limits>
#include <print>

#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <oran/async/sleep.hpp>
#include <oran/hook/bus.hpp>

namespace orangutan::telegram_host {
namespace {
using core::Error;
using core::Result;

permission::RuleSet rules() {
  permission::RuleSet result;
  for (const auto* operation : {"ChannelPoll", "ChannelState", "ChannelReceive", "ChannelSend", "ChannelTyping"})
    result.push_back({.verdict = permission::Verdict::allow, .tool_pattern = operation});
  return result;
}

async::Awaitable<Result<void>> authorize(hook::Bus& hooks, std::string operation, core::Capability capability) {
  const auto cancelled = co_await asio::this_coro::cancellation_state;
  if (cancelled.cancelled() != asio::cancellation_type::none)
    co_return std::unexpected(Error::cancelled());
  const std::array required{capability};
  auto decision = permission::evaluate(rules(), operation, required, permission::Mode::strict);
  co_await hooks.publish_advisory(hook::Event::channel_action,
                                  hook::ChannelActionPayload{"telegram",
                                                             "live-host",
                                                             std::move(operation),
                                                             decision.verdict == permission::Verdict::allow});
  if (decision.verdict != permission::Verdict::allow)
    co_return std::unexpected(Error::permission_denied("Telegram host operation denied"));
  co_return Result<void>{};
}

async::Awaitable<Result<void>> write_state(io::PrivateDirectory& directory, std::string bytes) {
  // PrivateDirectory writes are atomic and fsync both the file and directory.
  co_return directory.write("state.json", bytes);
}

async::Awaitable<Result<void>>
persist(io::PrivateDirectory& directory, const Json& state, hook::Bus& hooks, asio::any_io_executor worker) {
  auto allowed = co_await authorize(hooks, "ChannelState", core::Capability::write_file);
  if (!allowed)
    co_return std::unexpected(allowed.error());
  co_return co_await asio::co_spawn(worker, write_state(directory, state.dump(2) + "\n"), asio::use_awaitable);
}

async::Awaitable<Result<Json>> request(Api& api, hook::Bus& hooks, std::string path, Json body) {
  auto allowed = co_await authorize(hooks, "ChannelPoll", core::Capability::egress_http);
  if (!allowed)
    co_return std::unexpected(allowed.error());
  auto response = co_await api({"POST", std::move(path), body.dump()});
  if (!response)
    co_return std::unexpected(Error{response.error().kind(), "Telegram polling transport failed"});
  co_return api_result(*response);
}
}  // namespace

bool admits(const channel::Message& message, std::string_view user) {
  return message.conversation.platform == channel::Platform::telegram &&
         message.conversation.kind == channel::ChatKind::direct && message.sender == user &&
         message.conversation.chat == user && message.conversation.thread.empty();
}

Result<Json> api_result(const channel::Response& response) {
  auto parsed = Json::parse(response.body, nullptr, false);
  if (response.status < 200 || response.status >= 300 ||
      (parsed.is_object() && parsed.contains("ok") && parsed["ok"] == false)) {
    auto accepted = channel::telegram().accept(response);
    if (!accepted)
      return std::unexpected(accepted.error());
  }
  // getUpdates returns an array; the outbound adapter expects a message receipt.
  if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("result") || !parsed.contains("ok") ||
      parsed["ok"] != true)
    return std::unexpected(Error::parsing("invalid Telegram API response"));
  return parsed["result"];
}

Result<Json> load_state(const io::PrivateDirectory& directory, std::string_view user, std::string_view workspace) {
  auto bytes = directory.read("state.json", 2 * 1024 * 1024);
  if (!bytes)
    return std::unexpected(bytes.error());
  if (!*bytes) {
    auto id = core::generate_turn_id();
    if (!id)
      return std::unexpected(id.error());
    return Json{{"bot", ""},
                {"user", user},
                {"workspace", workspace},
                {"session", core::format_turn_id_hex(*id)},
                {"next_update", 0},
                {"pending", nullptr}};
  }
  try {
    auto state = Json::parse(**bytes);
    auto id = core::parse_turn_id_hex(state.at("session").get<std::string>());
    if (state.at("user") != user || state.at("workspace") != workspace || !state.at("bot").is_string() ||
        !state.at("next_update").is_number_integer() || state.at("next_update").get<std::int64_t>() < 0 || !id ||
        core::is_zero_turn_id(*id) || !state.at("pending").is_null())
      return std::unexpected(Error{core::ErrorKind::conflict,
                                   "state binding mismatch or unresolved pending delivery; inspect state.json"});
    return state;
  } catch (const Json::exception&) {
    return std::unexpected(Error::parsing("invalid Telegram state journal"));
  }
}

Result<void> acknowledge_pending(const io::PrivateDirectory& directory, std::int64_t update) {
  auto bytes = directory.read("state.json", 2 * 1024 * 1024);
  if (!bytes)
    return std::unexpected(bytes.error());
  if (!*bytes || update < 0 || update == std::numeric_limits<std::int64_t>::max())
    return std::unexpected(Error::invalid_argument("no matching pending update"));
  try {
    auto state = Json::parse(**bytes);
    if (state.at("pending").at("update").at("update_id") != update)
      return std::unexpected(Error{core::ErrorKind::conflict, "pending update differs from acknowledgment"});
    auto archived = directory.write("handled-" + std::to_string(update) + ".json", **bytes);
    if (!archived)
      return std::unexpected(archived.error());
    state["next_update"] = update + 1;
    state["pending"] = nullptr;
    return directory.write("state.json", state.dump(2) + "\n");
  } catch (const Json::exception&) {
    return std::unexpected(Error::parsing("invalid pending update journal"));
  }
}

static async::Awaitable<Result<void>> run_impl(Options options,
                                               Api api,
                                               channel::Transport outbound,
                                               channel::RunTurn turn,
                                               hook::Bus& hooks,
                                               io::PrivateDirectory& directory,
                                               Json& state,
                                               asio::any_io_executor worker) {
  auto me = co_await request(api, hooks, "/getMe", Json::object());
  if (!me)
    co_return std::unexpected(me.error());
  if (!me->is_object() || !me->value("is_bot", false) || !me->at("id").is_number_integer() ||
      !me->at("username").is_string())
    co_return std::unexpected(Error::parsing("invalid Telegram bot identity"));
  const auto bot = std::to_string(me->at("id").get<std::int64_t>());
  auto webhook = co_await request(api, hooks, "/getWebhookInfo", Json::object());
  if (!webhook)
    co_return std::unexpected(webhook.error());
  if (!webhook->at("url").get<std::string>().empty())
    co_return std::unexpected(Error{core::ErrorKind::conflict, "active Telegram webhook; polling refused"});
  std::println("Telegram bot @{} verified; webhook absent", me->at("username").get<std::string>());
  std::fflush(stdout);
  if (options.probe)
    co_return Result<void>{};
  if (state.at("bot") != "" && state.at("bot") != bot)
    co_return std::unexpected(Error{core::ErrorKind::conflict, "Telegram bot differs from saved binding"});
  state["bot"] = bot;
  auto saved = co_await persist(directory, state, hooks, worker);
  if (!saved)
    co_return std::unexpected(saved.error());

  // Save the answer before sending and every confirmed send before advancing.
  // A crash anywhere in this window leaves pending non-null and blocks restart.
  channel::Transport delivery = [&](channel::Conversation conversation,
                                    channel::Request outgoing) -> async::Awaitable<Result<channel::Response>> {
    const bool sending = outgoing.path == "/sendMessage";
    if (sending) {
      state["pending"]["send_inflight"] = true;
      auto written = co_await persist(directory, state, hooks, worker);
      if (!written)
        co_return std::unexpected(written.error());
    }
    auto response = co_await outbound(std::move(conversation), std::move(outgoing));
    auto receipt = response ? channel::telegram().accept(*response)
                            : Result<channel::Receipt>{std::unexpected(Error::network("send failed"))};
    if (sending && receipt && !receipt->id.empty()) {
      state["pending"]["confirmed_parts"] = state["pending"].value("confirmed_parts", 0) + 1;
      state["pending"]["send_inflight"] = false;
      auto written = co_await persist(directory, state, hooks, worker);
      if (!written)
        co_return std::unexpected(written.error());
    }
    co_return response;
  };
  channel::RunTurn execute = [&](channel::Message message) -> async::Awaitable<Result<std::string>> {
    auto answer = co_await turn(std::move(message));
    if (!answer)
      co_return std::unexpected(answer.error());
    state["pending"]["answer"] = *answer;
    auto written = co_await persist(directory, state, hooks, worker);
    if (!written)
      co_return std::unexpected(written.error());
    co_return answer;
  };
  channel::DispatcherOptions dispatcher_options;
  dispatcher_options.rules = rules();
  dispatcher_options.hooks = &hooks;
  auto dispatcher = channel::Dispatcher::create(channel::telegram(),
                                                std::move(delivery),
                                                std::move(execute),
                                                std::move(dispatcher_options));
  if (!dispatcher)
    co_return std::unexpected(dispatcher.error());
  std::println("Listening for private messages from user {}", options.user);
  std::fflush(stdout);
  unsigned failures = 0;
  for (;;) {
    auto updates = co_await request(api,
                                    hooks,
                                    "/getUpdates",
                                    Json{{"offset", state.at("next_update")},
                                         {"limit", 1},
                                         {"timeout", 25},
                                         {"allowed_updates", Json::array({"message"})}});
    if (!updates) {
      const auto kind = updates.error().kind();
      if (++failures >= 5 ||
          (kind != core::ErrorKind::network && kind != core::ErrorKind::rate_limit && kind != core::ErrorKind::timeout))
        co_return std::unexpected(updates.error());
      auto delay = updates.error().retry_after().value_or(std::chrono::seconds{2 * failures});
      auto waited = co_await async::sleep_for(co_await asio::this_coro::executor, delay);
      if (!waited)
        co_return std::unexpected(waited.error());
      continue;
    }
    failures = 0;
    if (!updates->is_array() || updates->size() > 1)
      co_return std::unexpected(Error::parsing("invalid Telegram update batch"));
    if (updates->empty())
      continue;
    const auto& update = updates->front();
    if (!update.at("update_id").is_number_integer())
      co_return std::unexpected(Error::parsing("invalid Telegram update identifier"));
    const auto id = update.at("update_id").get<std::int64_t>();
    if (id < state.at("next_update").get<std::int64_t>() || id == std::numeric_limits<std::int64_t>::max())
      co_return std::unexpected(Error::parsing("out-of-order Telegram update"));
    auto message = channel::telegram().decode(update.dump(), {bot, bot});
    if (!message)
      co_return std::unexpected(message.error());
    if (*message && admits(**message, options.user)) {
      state["pending"] =
          Json{{"update", update}, {"answer", nullptr}, {"confirmed_parts", 0}, {"send_inflight", false}};
      saved = co_await persist(directory, state, hooks, worker);
      if (!saved)
        co_return std::unexpected(saved.error());
      auto delivered = co_await (*dispatcher)->handle(std::move(**message));
      if (!delivered)
        co_return std::unexpected(delivered.error());
      std::println("Delivered update {} ({} parts)", id, delivered->parts_sent);
      std::fflush(stdout);
      state["pending"] = nullptr;
      state["next_update"] = id + 1;
      saved = co_await persist(directory, state, hooks, worker);
      if (!saved)
        co_return std::unexpected(saved.error());
      if (options.once)
        co_return Result<void>{};
    } else {
      state["next_update"] = id + 1;
      saved = co_await persist(directory, state, hooks, worker);
      if (!saved)
        co_return std::unexpected(saved.error());
    }
  }
}

async::Awaitable<Result<void>> run(Options options,
                                   Api api,
                                   channel::Transport outbound,
                                   channel::RunTurn turn,
                                   hook::Bus& hooks,
                                   io::PrivateDirectory& directory,
                                   Json& state,
                                   asio::any_io_executor worker) {
  try {
    co_return co_await run_impl(std::move(options),
                                std::move(api),
                                std::move(outbound),
                                std::move(turn),
                                hooks,
                                directory,
                                state,
                                worker);
  } catch (const Json::exception&) {
    co_return std::unexpected(Error::parsing("invalid Telegram host data"));
  } catch (...) {
    // Never propagate transport exception text, which may contain a token URL.
  }
  const auto cancelled = co_await asio::this_coro::cancellation_state;
  co_return std::unexpected(cancelled.cancelled() != asio::cancellation_type::none
                                ? Error::cancelled()
                                : Error::internal("Telegram host failed"));
}
}  // namespace orangutan::telegram_host
