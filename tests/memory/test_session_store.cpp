// tests/memory/test_session_store.cpp — typed session memory coverage.

#include <chrono>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/core/content.hpp>
#include <oran/memory.hpp>
#include <oran/storage.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace memory = orangutan::memory;
namespace storage = orangutan::storage;
namespace test = orangutan::tests;

namespace {

class TempDb {
public:
  explicit TempDb(std::string name)
      : path_(std::filesystem::temp_directory_path() /
              (std::move(name) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
               ".db")) {}

  ~TempDb() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    std::filesystem::remove(path_.string() + "-wal", ec);
    std::filesystem::remove(path_.string() + "-shm", ec);
  }

  TempDb(const TempDb&) = delete;
  TempDb& operator=(const TempDb&) = delete;

  [[nodiscard]] std::string string() const {
    return path_.string();
  }

private:
  std::filesystem::path path_;
};

storage::Pool open_pool(asio::io_context& io, TempDb& db) {
  auto pool =
      storage::Pool::open(io.get_executor(),
                          storage::PoolOptions{.path = db.string(), .reader_count = 2, .statement_cache_capacity = 8});
  REQUIRE(pool.has_value());
  return std::move(*pool);
}

core::Message full_message(core::Role role) {
  return core::Message{
      .role = role,
      .blocks =
          {
              core::TextContent{.text = "hello"},
              core::ThinkingContent{.thinking = "considering", .signature = std::string{"sig"}},
              core::ToolUseContent{.id = "toolu-1", .name = "file.read", .input_json = R"({"path":"README.md"})"},
              core::ToolResultContent{
                  .tool_use_id = "toolu-1",
                  .output = "README",
                  .data_json = std::string{R"({"kind":"file_read"})"},
                  .is_error = false,
              },
          },
      .created_at = std::nullopt,
  };
}

}  // namespace

TEST_CASE("session::Store appends and loads typed messages in order", "[unit][memory][session]") {
  TempDb db{"oran-memory-session-roundtrip"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::SessionRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());
    memory::session::Store store{repo};

    auto first = co_await store.append(memory::session::SessionId{.value = "s-1"},
                                       memory::session::AgentKey{.value = "coder"},
                                       core::Message::user_text("first"));
    REQUIRE(first.has_value());
    auto second = co_await store.append(memory::session::SessionId{.value = "s-1"},
                                        memory::session::AgentKey{.value = "coder"},
                                        full_message(core::Role::assistant));
    REQUIRE(second.has_value());

    auto loaded =
        co_await store.load(memory::session::SessionId{.value = "s-1"}, memory::session::AgentKey{.value = "coder"});
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->size() == 2);
    REQUIRE((*loaded)[0].role == core::Role::user);
    REQUIRE((*loaded)[0].blocks == core::Message::user_text("first").blocks);
    REQUIRE((*loaded)[0].created_at.has_value());
    REQUIRE((*loaded)[1].role == core::Role::assistant);
    REQUIRE((*loaded)[1].blocks == full_message(core::Role::assistant).blocks);
    REQUIRE((*loaded)[1].created_at.has_value());

    auto summaries = co_await store.list(memory::session::ListSessionsOptions{
        .agent_key = memory::session::AgentKey{.value = "coder"},
        .limit = 10,
    });
    REQUIRE(summaries.has_value());
    REQUIRE(summaries->size() == 1);
    REQUIRE((*summaries)[0].session_id.value == "s-1");
    REQUIRE((*summaries)[0].agent_key.value == "coder");
    REQUIRE((*summaries)[0].message_count == 2);
  });
}

TEST_CASE("session::Store keeps agents scoped apart", "[unit][memory][session]") {
  TempDb db{"oran-memory-session-scope"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::SessionRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());
    memory::session::Store store{repo};

    auto coder = co_await store.append(memory::session::SessionId{.value = "s-1"},
                                       memory::session::AgentKey{.value = "coder"},
                                       core::Message::user_text("coder"));
    REQUIRE(coder.has_value());
    auto researcher = co_await store.append(memory::session::SessionId{.value = "s-1"},
                                            memory::session::AgentKey{.value = "researcher"},
                                            core::Message::user_text("researcher"));
    REQUIRE(researcher.has_value());

    auto loaded_coder =
        co_await store.load(memory::session::SessionId{.value = "s-1"}, memory::session::AgentKey{.value = "coder"});
    REQUIRE(loaded_coder.has_value());
    REQUIRE(loaded_coder->size() == 1);
    REQUIRE((*loaded_coder)[0].blocks == core::Message::user_text("coder").blocks);

    auto loaded_researcher = co_await store.load(memory::session::SessionId{.value = "s-1"},
                                                 memory::session::AgentKey{.value = "researcher"});
    REQUIRE(loaded_researcher.has_value());
    REQUIRE(loaded_researcher->size() == 1);
    REQUIRE((*loaded_researcher)[0].blocks == core::Message::user_text("researcher").blocks);
  });
}

TEST_CASE("session::Store records durable skill activation state", "[unit][memory][session]") {
  TempDb db{"oran-memory-session-skill-activation"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::SessionRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());
    memory::session::Store store{repo};

    auto activated = co_await store.record_skill_activation(memory::session::SessionId{.value = "s-1"},
                                                            memory::session::AgentKey{.value = "coder"},
                                                            memory::session::SkillActivationUpdate{
                                                                .name = "release-note",
                                                                .active = true,
                                                            });
    REQUIRE(activated.has_value());
    auto deactivated = co_await store.record_skill_activation(memory::session::SessionId{.value = "s-1"},
                                                              memory::session::AgentKey{.value = "coder"},
                                                              memory::session::SkillActivationUpdate{
                                                                  .name = "release-note",
                                                                  .active = false,
                                                              });
    REQUIRE(deactivated.has_value());
    auto review = co_await store.record_skill_activation(memory::session::SessionId{.value = "s-1"},
                                                         memory::session::AgentKey{.value = "coder"},
                                                         memory::session::SkillActivationUpdate{
                                                             .name = "review-pr",
                                                             .active = true,
                                                         });
    REQUIRE(review.has_value());
    auto researcher = co_await store.record_skill_activation(memory::session::SessionId{.value = "s-1"},
                                                             memory::session::AgentKey{.value = "researcher"},
                                                             memory::session::SkillActivationUpdate{
                                                                 .name = "release-note",
                                                                 .active = true,
                                                             });
    REQUIRE(researcher.has_value());

    auto loaded = co_await store.load_skill_activations(memory::session::SessionId{.value = "s-1"},
                                                        memory::session::AgentKey{.value = "coder"});
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->size() == 2);
    REQUIRE((*loaded)[0].name == "release-note");
    REQUIRE_FALSE((*loaded)[0].active);
    REQUIRE_FALSE((*loaded)[0].created_at.empty());
    REQUIRE_FALSE((*loaded)[0].updated_at.empty());
    REQUIRE((*loaded)[1].name == "review-pr");
    REQUIRE((*loaded)[1].active);
  });
}

TEST_CASE("session::Store round-trips 500 messages", "[unit][memory][session]") {
  TempDb db{"oran-memory-session-500"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::SessionRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());
    memory::session::Store store{repo};

    constexpr std::size_t message_count = 500;
    for (std::size_t i = 0; i < message_count; ++i) {
      auto appended = co_await store.append(memory::session::SessionId{.value = "s-500"},
                                            memory::session::AgentKey{.value = "coder"},
                                            core::Message::user_text("message-" + std::to_string(i)));
      REQUIRE(appended.has_value());
    }

    auto loaded =
        co_await store.load(memory::session::SessionId{.value = "s-500"}, memory::session::AgentKey{.value = "coder"});
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->size() == message_count);
    REQUIRE((*loaded).front().blocks == core::Message::user_text("message-0").blocks);
    REQUIRE((*loaded).back().blocks == core::Message::user_text("message-499").blocks);
  });
}

TEST_CASE("session::Store rejects malformed stored message JSON", "[unit][memory][session]") {
  TempDb db{"oran-memory-session-malformed"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::SessionRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    auto appended = co_await repo.append_message(storage::AppendSessionMessageRequest{
        .session_id = "s-1",
        .agent_key = "coder",
        .role = core::Role::assistant,
        .content_json = R"({"version":1,"blocks":[{"type":"tool_result","tool_use_id":"u","output":"x"}]})",
    });
    REQUIRE(appended.has_value());

    memory::session::Store store{repo};
    auto loaded =
        co_await store.load(memory::session::SessionId{.value = "s-1"}, memory::session::AgentKey{.value = "coder"});
    REQUIRE_FALSE(loaded.has_value());
    REQUIRE(loaded.error().kind() == core::ErrorKind::parsing);
  });
}

TEST_CASE("session::Store validates required ids", "[unit][memory][session]") {
  TempDb db{"oran-memory-session-validation"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::SessionRepository repo{pool};
    memory::session::Store store{repo};

    auto append = co_await store.append(memory::session::SessionId{.value = ""},
                                        memory::session::AgentKey{.value = "coder"},
                                        core::Message::user_text("x"));
    REQUIRE_FALSE(append.has_value());
    REQUIRE(append.error().kind() == core::ErrorKind::invalid_argument);

    auto load = co_await store.load(memory::session::SessionId{.value = "s-1"}, memory::session::AgentKey{.value = ""});
    REQUIRE_FALSE(load.has_value());
    REQUIRE(load.error().kind() == core::ErrorKind::invalid_argument);

    auto list = co_await store.list(memory::session::ListSessionsOptions{
        .agent_key = memory::session::AgentKey{.value = ""},
    });
    REQUIRE_FALSE(list.has_value());
    REQUIRE(list.error().kind() == core::ErrorKind::invalid_argument);

    auto skill = co_await store.record_skill_activation(memory::session::SessionId{.value = "s-1"},
                                                        memory::session::AgentKey{.value = "coder"},
                                                        memory::session::SkillActivationUpdate{
                                                            .name = "release\nnote",
                                                            .active = true,
                                                        });
    REQUIRE_FALSE(skill.has_value());
    REQUIRE(skill.error().kind() == core::ErrorKind::invalid_argument);
  });
}
