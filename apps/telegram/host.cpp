#include "host.hpp"
#include "format.hpp"
#include "presentation.hpp"

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
  const auto cancellation = co_await asio::this_coro::cancellation_state;
  if (cancellation.cancelled() != asio::cancellation_type::none)
    co_return std::unexpected(Error::cancelled());
  // PrivateDirectory writes are atomic and fsync both the file and directory.
  co_return directory.write("state.json", bytes);
}

async::Awaitable<Result<void>>
persist(io::PrivateDirectory& directory, const State& state, hook::Bus& hooks, asio::any_io_executor worker) {
  auto allowed = co_await authorize(hooks, "ChannelState", core::Capability::write_file);
  if (!allowed)
    co_return std::unexpected(allowed.error());
  co_return co_await asio::co_spawn(worker,
                                    write_state(directory, encode_state(state).dump(2) + "\n"),
                                    asio::use_awaitable);
}

async::Awaitable<Result<Json>> request(Api& api, hook::Bus& hooks, PollMethod method, Json body) {
  auto allowed = co_await authorize(hooks, "ChannelPoll", core::Capability::egress_http);
  if (!allowed)
    co_return std::unexpected(allowed.error());
  auto response = co_await api(method, body.dump());
  if (!response) {
    Error error{response.error().kind(), "Telegram polling transport failed"};
    if (response.error().retry_after())
      error.with_retry_after(*response.error().retry_after());
    co_return std::unexpected(std::move(error));
  }
  co_return api_result(*response);
}
}  // namespace

bool admits(const channel::Message& message, std::string_view user) {
  return message.conversation.platform == channel::Platform::telegram &&
         message.conversation.kind == channel::ChatKind::direct && message.sender == user &&
         message.conversation.chat == user && message.conversation.thread.empty();
}

Result<Json> api_result(const channel::Response& response) {
  if (response.status == 409)
    return std::unexpected(Error{core::ErrorKind::conflict, "another Telegram poller is active"});
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

static async::Awaitable<Result<void>> run_impl(Options options,
                                               Api api,
                                               channel::Transport outbound,
                                               channel::RunTurn turn,
                                               hook::Bus& hooks,
                                               io::PrivateDirectory& directory,
                                               State& state,
                                               asio::any_io_executor worker,
                                               Presentation* presentation) {
  auto me = co_await request(api, hooks, PollMethod::getMe, Json::object());
  if (!me)
    co_return std::unexpected(me.error());
  if (!me->is_object() || !me->value("is_bot", false) || !me->at("id").is_number_integer() ||
      !me->at("username").is_string())
    co_return std::unexpected(Error::parsing("invalid Telegram bot identity"));
  const auto bot = std::to_string(me->at("id").get<std::int64_t>());
  auto webhook = co_await request(api, hooks, PollMethod::getWebhookInfo, Json::object());
  if (!webhook)
    co_return std::unexpected(webhook.error());
  if (!webhook->at("url").get<std::string>().empty())
    co_return std::unexpected(Error{core::ErrorKind::conflict, "active Telegram webhook; polling refused"});
  std::println("Telegram bot @{} verified; webhook absent", me->at("username").get<std::string>());
  std::fflush(stdout);
  if (options.probe)
    co_return Result<void>{};
  if (!state.bot.empty() && state.bot != bot)
    co_return std::unexpected(Error{core::ErrorKind::conflict, "Telegram bot differs from saved binding"});
  state.bot = bot;
  auto saved = co_await persist(directory, state, hooks, worker);
  if (!saved)
    co_return std::unexpected(saved.error());

  // Save the answer before sending and every confirmed send before advancing.
  // A crash anywhere in this window leaves pending non-null and blocks restart.
  std::vector<FormattedText> final_parts;
  channel::Transport delivery = [&](channel::Conversation conversation,
                                    channel::Request outgoing) -> async::Awaitable<Result<channel::Response>> {
    const bool sending = outgoing.path == "/sendMessage";
    if (sending) {
      state.pending->send_inflight = true;
      auto written = co_await persist(directory, state, hooks, worker);
      if (!written)
        co_return std::unexpected(written.error());
    }
    auto response = co_await outbound(std::move(conversation), std::move(outgoing));
    if (!sending || !response)
      co_return response;
    auto receipt = channel::telegram().accept(*response);
    if (receipt && !receipt->id.empty()) {
      ++state.pending->confirmed_parts;
      state.pending->send_inflight = false;
      auto written = co_await persist(directory, state, hooks, worker);
      if (!written)
        co_return std::unexpected(written.error());
    }
    co_return response;
  };
  channel::RunTurn execute = [&](channel::Message message) -> async::Awaitable<Result<std::string>> {
    if (presentation)
      presentation->accepted();
    auto answer = co_await turn(std::move(message));
    if (presentation) {
      auto drained = co_await presentation->prepare_final();
      if (!drained && answer)
        co_return std::unexpected(drained.error());
    }
    if (!answer)
      co_return std::unexpected(answer.error());
    state.pending->answer = *answer;
    auto written = co_await persist(directory, state, hooks, worker);
    if (!written)
      co_return std::unexpected(written.error());
    auto formatted = format_markdown(*answer);
    if (!formatted)
      co_return std::unexpected(formatted.error());
    auto parts = split_formatted(*formatted, channel::telegram().capabilities().text_bytes);
    if (!parts)
      co_return std::unexpected(parts.error());
    final_parts = std::move(*parts);
    co_return std::move(formatted->text);
  };
  channel::DispatcherOptions dispatcher_options;
  dispatcher_options.rules = rules();
  dispatcher_options.hooks = &hooks;
  dispatcher_options.render_reply =
      [&](const channel::Message& message, std::string_view text, std::size_t part) -> Result<channel::Request> {
    auto request = channel::telegram().reply(message, text, part);
    if (!request)
      return std::unexpected(request.error());
    auto body = Json::parse(request->body);
    body["entities"] = final_parts[part].entities;
    request->body = body.dump();
    return request;
  };
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
                                    PollMethod::getUpdates,
                                    Json{{"offset", state.next_update},
                                         {"limit", 1},
                                         {"timeout", 25},
                                         {"allowed_updates", Json::array({"message"})}});
    if (!updates) {
      if (++failures >= 5 || !updates.error().retryable())
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
    if (id < state.next_update || id == std::numeric_limits<std::int64_t>::max())
      co_return std::unexpected(Error::parsing("out-of-order Telegram update"));
    auto message = channel::telegram().decode(update.dump(), {bot, bot});
    if (!message)
      co_return std::unexpected(message.error());
    const bool admitted = *message && admits(**message, options.user);
    if (admitted) {
      state.pending = Pending{.update = update, .answer = std::nullopt};
      saved = co_await persist(directory, state, hooks, worker);
      if (!saved)
        co_return std::unexpected(saved.error());
      auto delivered = presentation ? co_await presentation->deliver(**dispatcher, std::move(**message))
                                    : co_await (*dispatcher)->handle(std::move(**message));
      if (!delivered)
        co_return std::unexpected(delivered.error());
      std::println("Delivered update {} ({} parts)", id, delivered->parts_sent);
      std::fflush(stdout);
      state.pending.reset();
    }
    state.next_update = id + 1;
    saved = co_await persist(directory, state, hooks, worker);
    if (!saved)
      co_return std::unexpected(saved.error());
    if (admitted && options.once)
      co_return Result<void>{};
  }
}

async::Awaitable<Result<void>> run(Options options,
                                   Api api,
                                   channel::Transport outbound,
                                   channel::RunTurn turn,
                                   hook::Bus& hooks,
                                   io::PrivateDirectory& directory,
                                   State& state,
                                   asio::any_io_executor worker,
                                   Presentation* presentation) {
  try {
    co_return co_await run_impl(std::move(options),
                                std::move(api),
                                std::move(outbound),
                                std::move(turn),
                                hooks,
                                directory,
                                state,
                                worker,
                                presentation);
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
