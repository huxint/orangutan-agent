#include "../test-helpers/run_async.hpp"
#include <asio/awaitable.hpp>
#include <catch2/catch_test_macros.hpp>
#include <oran/bootstrap/channel_http.hpp>
#include <oran/core/error.hpp>
#include <oran/http/client.hpp>

TEST_CASE("Channel HTTP binding rejects unsafe routes before looking up credentials") {
  orangutan::tests::run_async([](asio::io_context& io) -> orangutan::async::Awaitable<void> {
    orangutan::http::Client client{io.get_executor()};
    int lookups = 0;
    auto transport = orangutan::bootstrap::channel_http_transport(
        client,
        [&](orangutan::channel::Conversation) -> orangutan::async::Awaitable<orangutan::core::Result<std::string>> {
          ++lookups;
          co_return std::unexpected(orangutan::core::Error::config("SECRET"));
        });
    orangutan::channel::Conversation conversation{orangutan::channel::Platform::telegram,
                                                  "bot",
                                                  orangutan::channel::ChatKind::direct,
                                                  "1",
                                                  {}};
    for (const std::string path : {"//evil.test", "/sendMessage?token=bad", "/deleteWebhook", "/../sendMessage"}) {
      auto result = co_await transport(conversation, {"POST", path, "{}"});
      REQUIRE_FALSE(result);
      CHECK(lookups == 0);
    }
    auto result = co_await transport(conversation, {"POST", "/sendMessage", "{}"});
    REQUIRE_FALSE(result);
    CHECK(lookups == 1);
    CHECK(result.error().kind() == orangutan::core::ErrorKind::auth);
    CHECK_FALSE(result.error().message().contains("SECRET"));
    for (const std::string path : {"/setMessageReaction", "/sendMessageDraft"}) {
      auto supported = co_await transport(conversation, {"POST", path, "{}"});
      REQUIRE_FALSE(supported);
      CHECK(supported.error().kind() == orangutan::core::ErrorKind::auth);
    }
    CHECK(lookups == 3);
    auto cancelled_transport = orangutan::bootstrap::channel_http_transport(
        client,
        [](orangutan::channel::Conversation) -> orangutan::async::Awaitable<orangutan::core::Result<std::string>> {
          co_return std::unexpected(orangutan::core::Error::cancelled());
        });
    auto cancelled = co_await cancelled_transport(conversation, {"POST", "/sendMessage", "{}"});
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().kind() == orangutan::core::ErrorKind::cancelled);
  });
}
