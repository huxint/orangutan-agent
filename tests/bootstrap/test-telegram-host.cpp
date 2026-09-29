#include "../../apps/telegram/host.hpp"
#include "../test-helpers/run_async.hpp"

#include <filesystem>
#include <stdexcept>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/post.hpp>
#include <oran/async/sleep.hpp>
#include <oran/hook/bus.hpp>

namespace {
using namespace orangutan;
using core::Result;
using telegram_host::Json;

struct StateDirectory {
  std::filesystem::path path;
  StateDirectory() {
    auto id = core::generate_turn_id();
    REQUIRE(id);
    path = std::filesystem::temp_directory_path() / ("oran-telegram-" + core::format_turn_id_hex(*id));
  }
  ~StateDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

channel::Response response(Json result) {
  return {200, Json{{"ok", true}, {"result", std::move(result)}}.dump(), {}};
}
Json update(int id, int user = 42, std::string kind = "private") {
  return {{"update_id", id},
          {"message",
           {{"message_id", id},
            {"text", "hello"},
            {"from", {{"id", user}, {"is_bot", false}}},
            {"chat", {{"id", user}, {"type", std::move(kind)}}}}}};
}
telegram_host::Api api(std::vector<Json> updates, std::vector<std::int64_t>& offsets, bool webhook = false) {
  return [updates = std::move(updates), &offsets, webhook, index = std::size_t{}](
             channel::Request request) mutable -> async::Awaitable<Result<channel::Response>> {
    if (request.path == "/getMe")
      co_return response(Json{{"is_bot", true}, {"id", 123}, {"username", "fixture_bot"}});
    if (request.path == "/getWebhookInfo")
      co_return response(Json{{"url", webhook ? "https://example.test/hook" : ""}});
    REQUIRE(request.path == "/getUpdates");
    auto body = Json::parse(request.body);
    CHECK(body.at("limit") == 1);
    offsets.push_back(body.at("offset").get<std::int64_t>());
    REQUIRE(index < updates.size());
    co_return response(Json::array({updates[index++]}));
  };
}
}  // namespace

TEST_CASE("Telegram live host filters senders and groups and resumes its saved cursor", "[telegram-host]") {
  StateDirectory temp;
  auto directory = io::PrivateDirectory::open(temp.path.string());
  REQUIRE(directory);
  auto state = telegram_host::load_state(*directory, "42", "/workspace");
  REQUIRE(state);
  auto session = state->at("session");
  tests::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    hook::Bus hooks;
    int turns = 0, sends = 0;
    channel::RunTurn turn = [&](channel::Message message) -> async::Awaitable<Result<std::string>> {
      CHECK(message.sender == "42");
      auto persisted = directory->read("state.json", 2 * 1024 * 1024);
      REQUIRE(persisted);
      CHECK_FALSE(Json::parse(**persisted).at("pending").is_null());
      ++turns;
      co_return "model answer";
    };
    channel::Transport send = [&](channel::Conversation,
                                  channel::Request request) -> async::Awaitable<Result<channel::Response>> {
      if (request.path == "/sendMessage") {
        ++sends;
        auto persisted = directory->read("state.json", 2 * 1024 * 1024);
        REQUIRE(persisted);
        auto pending = Json::parse(**persisted).at("pending");
        CHECK(pending.at("answer") == "model answer");
        CHECK(pending.at("send_inflight") == true);
        co_return response(Json{{"message_id", 100}});
      }
      co_return response(true);
    };
    std::vector<std::int64_t> offsets;
    auto first_api = api({update(10, 9), update(11, 42, "group"), update(12)}, offsets);
    auto first = co_await telegram_host::run({.user = "42", .once = true},
                                             std::move(first_api),
                                             send,
                                             turn,
                                             hooks,
                                             *directory,
                                             *state,
                                             io.get_executor());
    REQUIRE(first);
    CHECK(turns == 1);
    CHECK(sends == 1);
    CHECK(offsets == std::vector<std::int64_t>{0, 11, 12});
    auto reopened = telegram_host::load_state(*directory, "42", "/workspace");
    REQUIRE(reopened);
    CHECK(reopened->at("session") == session);
    CHECK(reopened->at("next_update") == 13);
    CHECK_FALSE(telegram_host::load_state(*directory, "43", "/workspace"));
    CHECK_FALSE(telegram_host::load_state(*directory, "42", "/other-workspace"));
    state = std::move(reopened);
    offsets.clear();
    auto second_api = api({update(13)}, offsets);
    auto second = co_await telegram_host::run({.user = "42", .once = true},
                                              std::move(second_api),
                                              send,
                                              turn,
                                              hooks,
                                              *directory,
                                              *state,
                                              io.get_executor());
    REQUIRE(second);
    CHECK(offsets == std::vector<std::int64_t>{13});
    CHECK(turns == 2);
  });
}

TEST_CASE("Telegram live host preserves ambiguous sends and blocks replay after restart", "[telegram-host]") {
  StateDirectory temp;
  auto directory = io::PrivateDirectory::open(temp.path.string());
  REQUIRE(directory);
  auto state = telegram_host::load_state(*directory, "42", "/workspace");
  REQUIRE(state);
  tests::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    hook::Bus hooks;
    std::vector<std::int64_t> offsets;
    auto failed_api = api({update(10)}, offsets);
    auto failed = co_await telegram_host::run(
        {.user = "42", .once = true},
        std::move(failed_api),
        [](channel::Conversation, channel::Request request) -> async::Awaitable<Result<channel::Response>> {
          if (request.path == "/sendMessage")
            co_return std::unexpected(core::Error::network("PRIVATE_URL"));
          co_return response(true);
        },
        [](channel::Message) -> async::Awaitable<Result<std::string>> { co_return std::string(5000, 'x'); },
        hooks,
        *directory,
        *state,
        io.get_executor());
    REQUIRE_FALSE(failed);
    CHECK_FALSE(failed.error().message().contains("PRIVATE_URL"));
    auto persisted = directory->read("state.json", 2 * 1024 * 1024);
    REQUIRE(persisted);
    auto journal = Json::parse(**persisted);
    CHECK(journal.at("next_update") == 0);
    CHECK(journal.at("pending").at("update").at("update_id") == 10);
    CHECK(journal.at("pending").at("answer").get<std::string>().size() == 5000);
    CHECK(journal.at("pending").at("send_inflight") == true);
    CHECK_FALSE(telegram_host::load_state(*directory, "42", "/workspace"));
    CHECK_FALSE(telegram_host::acknowledge_pending(*directory, 11));
    REQUIRE(telegram_host::acknowledge_pending(*directory, 10));
    auto reconciled = telegram_host::load_state(*directory, "42", "/workspace");
    REQUIRE(reconciled);
    CHECK(reconciled->at("next_update") == 11);
    auto archived = directory->read("handled-10.json", 2 * 1024 * 1024);
    REQUIRE(archived);
    CHECK(Json::parse(**archived).at("pending").at("answer") == std::string(5000, 'x'));
  });
}

TEST_CASE("Telegram live host refuses webhooks and hides transport failures", "[telegram-host]") {
  StateDirectory temp;
  auto directory = io::PrivateDirectory::open(temp.path.string());
  REQUIRE(directory);
  auto state = telegram_host::load_state(*directory, "42", "/workspace");
  REQUIRE(state);
  tests::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    hook::Bus hooks;
    std::vector<std::int64_t> offsets;
    auto refused = co_await telegram_host::run({.user = "42"},
                                               api({}, offsets, true),
                                               {},
                                               {},
                                               hooks,
                                               *directory,
                                               *state,
                                               io.get_executor());
    REQUIRE_FALSE(refused);
    CHECK(refused.error().kind() == core::ErrorKind::conflict);
    CHECK(offsets.empty());
    auto hidden = co_await telegram_host::run(
        {.probe = true},
        [](channel::Request) -> async::Awaitable<Result<channel::Response>> {
          throw std::runtime_error("PRIVATE_TOKEN");
          co_return response(true);
        },
        {},
        {},
        hooks,
        *directory,
        *state,
        io.get_executor());
    REQUIRE_FALSE(hidden);
    CHECK_FALSE(hidden.error().message().contains("PRIVATE_TOKEN"));
  });
  auto limited = telegram_host::api_result(
      {429, R"({"ok":false,"error_code":429,"description":"PRIVATE_TOKEN","parameters":{"retry_after":3}})", {}});
  REQUIRE_FALSE(limited);
  CHECK(limited.error().retry_after() == std::chrono::seconds{3});
  CHECK_FALSE(limited.error().message().contains("PRIVATE_TOKEN"));
}

TEST_CASE("Telegram live host joins cancellation and leaves intake recoverable", "[telegram-host]") {
  StateDirectory temp;
  auto directory = io::PrivateDirectory::open(temp.path.string());
  REQUIRE(directory);
  auto state = telegram_host::load_state(*directory, "42", "/workspace");
  REQUIRE(state);
  asio::io_context io;
  hook::Bus hooks;
  asio::cancellation_signal cancellation;
  std::vector<std::int64_t> offsets;
  bool returned = false, joined = false;
  asio::steady_timer deadline{io, std::chrono::seconds{2}};
  deadline.async_wait([&](asio::error_code error) {
    if (!error)
      io.stop();
  });
  asio::co_spawn(io,
                 telegram_host::run(
                     {.user = "42"},
                     api({update(10)}, offsets),
                     [](channel::Conversation, channel::Request) -> async::Awaitable<Result<channel::Response>> {
                       co_return response(true);
                     },
                     [&](channel::Message) -> async::Awaitable<Result<std::string>> {
                       asio::post(io, [&] { cancellation.emit(asio::cancellation_type::all); });
                       auto waited = co_await async::sleep_for(io.get_executor(), std::chrono::seconds{10});
                       joined = true;
                       if (!waited)
                         co_return std::unexpected(waited.error());
                       co_return "unexpected";
                     },
                     hooks,
                     *directory,
                     *state,
                     io.get_executor()),
                 asio::bind_cancellation_slot(cancellation.slot(), [&](std::exception_ptr error, Result<void> result) {
                   CHECK_FALSE(error);
                   CHECK_FALSE(result);
                   returned = true;
                   deadline.cancel();
                 }));
  io.run();
  CHECK(returned);
  CHECK(joined);
  CHECK_FALSE(telegram_host::load_state(*directory, "42", "/workspace"));
}
