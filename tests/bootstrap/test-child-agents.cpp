#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/this_coro.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <oran/agent.hpp>
#include <oran/async.hpp>
#include <oran/bootstrap.hpp>
#include <oran/config.hpp>
#include <oran/hook.hpp>
#include <oran/memory.hpp>
#include <oran/permission.hpp>
#include <oran/storage.hpp>
#include <oran/tool.hpp>

#include "../test-helpers/run_async.hpp"

namespace agent = orangutan::agent;
namespace async = orangutan::async;
namespace bootstrap = orangutan::bootstrap;
namespace config = orangutan::config;
namespace core = orangutan::core;
namespace hook = orangutan::hook;
namespace memory = orangutan::memory;
namespace permission = orangutan::permission;
namespace provider = orangutan::provider;
namespace storage = orangutan::storage;
namespace tool = orangutan::tool;
using namespace std::chrono_literals;

namespace {

constexpr std::string_view COLLABORATION_CONFIG = R"({
  "permissions":{"allow":[{"tool_pattern":"*"}]},
  "agents":{"parent":{"prompt_overlay":"Parent instructions"},"worker":{"prompt_overlay":"Worker instructions"}}
})";

class Workspace {
public:
  Workspace()
      : path{std::filesystem::temp_directory_path() /
             ("oran-child-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))} {
    std::filesystem::create_directories(path);
  }
  ~Workspace() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  Workspace(const Workspace&) = delete;
  Workspace& operator=(const Workspace&) = delete;
  std::filesystem::path path;
};

provider::Response answer(std::string text) {
  provider::Response response;
  response.blocks.emplace_back(core::TextContent{.text = std::move(text)});
  response.stop_reason = core::StopReason::end_turn;
  return response;
}

core::ToolUseContent call(std::string id, std::string name, std::string input) {
  return {.id = std::move(id), .name = std::move(name), .input_json = std::move(input)};
}

provider::Response calls(std::vector<core::Content> blocks) {
  provider::Response response;
  response.blocks = std::move(blocks);
  response.stop_reason = core::StopReason::tool_use;
  return response;
}

class ScriptedProvider final : public provider::System {
public:
  explicit ScriptedProvider(std::vector<provider::Response> responses) : responses_{std::move(responses)} {}

  async::Awaitable<core::Result<provider::Response>>
  send(provider::Request request, provider::Route, provider::EventSink* = nullptr) const override {
    requests.push_back(std::move(request));
    if (requests.size() > responses_.size()) {
      co_return std::unexpected(core::Error::internal("child test provider script exhausted"));
    }
    co_return responses_[requests.size() - 1];
  }

  mutable std::vector<provider::Request> requests;

private:
  std::vector<provider::Response> responses_;
};

config::Config parse_config(std::string_view json) {
  auto parsed = config::Config::parse(json, {.strict_unknown_fields = true});
  REQUIRE(parsed.has_value());
  return std::move(*parsed);
}

bootstrap::RuntimeAssembly build_assembly(const Workspace& workspace, asio::any_io_executor executor) {
  auto built = bootstrap::RuntimeAssembly::build(workspace.path.string(), std::move(executor));
  REQUIRE(built.has_value());
  return std::move(*built);
}

struct SessionFixture {
  explicit SessionFixture(asio::any_io_executor executor, std::string_view json = COLLABORATION_CONFIG)
      : executor{std::move(executor)}, config{parse_config(json)}, assembly{build_assembly(workspace, this->executor)} {
  }

  bootstrap::AgentSessionOptions options(provider::System& provider) {
    bootstrap::AgentSessionOptions options;
    options.executor = executor;
    options.blocking_executor = executor;
    options.assembly = &assembly;
    options.config = &config;
    options.provider = &provider;
    options.route.primary.profile = "test";
    options.route.primary.model = "test-model";
    options.agent_config_name = "parent";
    options.agent_key = "parent";
    options.scope_key = "scope-A";
    options.identity = "owner";
    options.mode = permission::Mode::strict;
    options.session_id.back() = std::byte{0x42};
    options.stream = false;
    options.retry.max_attempts = 1;
    return options;
  }

  std::int64_t worker_session_count() const {
    auto connection = storage::Connection::open({.path = std::string{assembly.sessions_path()},
                                                 .mode = storage::OpenMode::read_only});
    REQUIRE(connection.has_value());
    auto statement = connection->prepare("SELECT COUNT(*) FROM sessions WHERE agent_key = 'worker'");
    REQUIRE(statement.has_value());
    auto row = statement->step();
    REQUIRE(row.has_value());
    REQUIRE(*row == storage::StepResult::row);
    auto count = statement->column_int64(0);
    REQUIRE(count.has_value());
    return *count;
  }

  asio::any_io_executor executor;
  Workspace workspace;
  config::Config config;
  bootstrap::RuntimeAssembly assembly;
};

const core::ToolResultContent& result_in(const provider::Request& request, std::string_view id) {
  for (const auto& block : request.messages.back().blocks) {
    if (const auto* result = std::get_if<core::ToolResultContent>(&block); result && result->tool_use_id == id) {
      return *result;
    }
  }
  FAIL("expected tool result is missing");
  std::unreachable();
}

std::string policies(std::string_view parent, std::string_view child) {
  return nlohmann::json{{"agents",
                         {{"parent", {{"permissions", nlohmann::json::parse(parent)}}},
                          {"worker", {{"permissions", nlohmann::json::parse(child)}}}}}}
      .dump();
}

}  // namespace

TEST_CASE("AgentRun delivers a scoped child result with independent persisted history",
          "[integration][bootstrap][collaboration]") {
  bool explicit_catalog = false;
  SECTION("default active tools") {}
  SECTION("explicit active tools") {
    explicit_catalog = true;
  }
  orangutan::tests::run_async([explicit_catalog](asio::io_context& io) -> async::Awaitable<void> {
    auto configuration = nlohmann::json::parse(COLLABORATION_CONFIG);
    if (explicit_catalog) {
      configuration["runtime"]["prompt"]["active_tools"] = {"AgentRun", "MemoryRecall", "MemoryRemember"};
    }
    SessionFixture fixture{io.get_executor(), configuration.dump()};
    for (const auto& scope : {"scope-A", "scope-B"}) {
      memory::longterm::Record record;
      record.key = {.id = "note", .scope_key = scope};
      record.kind = memory::longterm::RecordKind::project;
      record.title = "sharedanchor";
      record.body = std::string{"sharedanchor in "} + scope;
      auto stored = co_await fixture.assembly.longterm_memory_backend()->upsert({.record = std::move(record)});
      REQUIRE(stored.has_value());
    }
    std::vector<hook::ToolAfterPayload> events;
    hook::InProcessSink capture{
        "child-tools",
        [&events](hook::Event, hook::PayloadPtr payload) -> async::Awaitable<core::Result<void>> {
          events.push_back(std::get<hook::ToolAfterPayload>(*payload));
          co_return core::Result<void>{};
        }};
    fixture.assembly.hook_bus().bind(capture, {hook::Event::tool_after});
    ScriptedProvider provider{{
        answer("saved parent answer"),
        calls({call("child-1", "AgentRun", R"({"agent":"worker","prompt":"inspect"})")}),
        calls({call("recall-1", "MemoryRecall", R"({"query":"sharedanchor"})")}),
        answer("child found the note"),
        answer("parent received the finding"),
    }};
    auto session = bootstrap::AgentSession::create(fixture.options(provider));
    REQUIRE(session.has_value());
    auto prior = co_await (*session)->run_prompt({.prompt = "parent history"});
    REQUIRE(prior.has_value());

    core::TurnId parent_turn{};
    parent_turn.back() = std::byte{0x91};
    auto result = co_await (*session)->run_prompt({.prompt = "delegate", .turn_id = parent_turn});

    REQUIRE(result.has_value());
    REQUIRE(result->text == "parent received the finding");
    REQUIRE(provider.requests.size() == 5);
    REQUIRE(std::ranges::contains(provider.requests[1].tools, std::string{"AgentRun"}, &core::ToolDef::name));
    REQUIRE_FALSE(provider.requests[1].system_prompt->contains("Tool: AgentRun"));
    const auto& child_request = provider.requests[2];
    REQUIRE(child_request.messages.size() == 1);
    REQUIRE(child_request.messages[0].blocks == core::Message::user_text("inspect").blocks);
    REQUIRE(child_request.system_prompt.has_value());
    REQUIRE(child_request.system_prompt->contains("Worker instructions"));
    REQUIRE(child_request.system_prompt->contains("Memory index:"));
    REQUIRE(child_request.system_prompt->contains("sharedanchor in scope-A"));
    REQUIRE_FALSE(child_request.system_prompt->contains("sharedanchor in scope-B"));
    REQUIRE_FALSE(child_request.system_prompt->contains("Parent instructions"));
    REQUIRE_FALSE(std::ranges::contains(child_request.tools, std::string{"AgentRun"}, &core::ToolDef::name));
    const auto& recall = result_in(provider.requests[3], "recall-1");
    REQUIRE_FALSE(recall.is_error);
    REQUIRE(recall.output.contains("sharedanchor in scope-A"));
    REQUIRE_FALSE(recall.output.contains("scope-B"));
    const auto& returned = result_in(provider.requests[4], "child-1");
    REQUIRE_FALSE(returned.is_error);
    REQUIRE(returned.output == "child found the note");
    REQUIRE(returned.data_json.has_value());
    const auto metadata = nlohmann::json::parse(*returned.data_json);
    const auto child_session_id = metadata["session_id"].get<std::string>();
    REQUIRE(fixture.worker_session_count() == 1);
    REQUIRE(child_session_id != "00000000000000000000000000000042");
    auto history = co_await fixture.assembly.session_store()->load({.value = child_session_id}, {.value = "worker"});
    REQUIRE(history.has_value());
    REQUIRE(history->size() == 4);
    REQUIRE(history->front().blocks == core::Message::user_text("inspect").blocks);
    REQUIRE(history->back().blocks == core::Message::assistant_text("child found the note").blocks);
    auto parent_history = co_await fixture.assembly.session_store()->load({.value = "00000000000000000000000000000042"},
                                                                          {.value = "parent"});
    REQUIRE(parent_history.has_value());
    REQUIRE(parent_history->size() == 6);
    auto traces = co_await fixture.assembly.trace_repository()->list_turns({.agent_key = "worker"});
    REQUIRE(traces.has_value());
    REQUIRE(traces->size() == 1);
    REQUIRE(traces->front().parent_turn_id == parent_turn);
    REQUIRE(core::format_turn_id_hex(traces->front().session_id) == metadata["session_id"].get<std::string>());
    REQUIRE(events.size() == 5);
    CHECK(std::ranges::count_if(events, [](const auto& event) {
            return event.tool_name == "MemoryRecall" && event.who.agent_key == "parent";
          }) == 2);
    CHECK(std::ranges::count_if(events, [](const auto& event) {
            return event.tool_name == "MemoryRecall" && event.who.agent_key == "worker";
          }) == 2);
    for (const auto& event : events) {
      CHECK(event.who.scope_key == "scope-A");
      if (event.who.agent_key == "worker") {
        CHECK(event.who.identity != "owner");
      } else {
        CHECK(event.who.identity == "owner");
      }
    }
    REQUIRE(events.back().tool_name == "AgentRun");
    REQUIRE(events.back().who.agent_key == "parent");
  });
}

TEST_CASE("a child memory index cannot exceed its parent's read permission",
          "[integration][bootstrap][collaboration][memory]") {
  orangutan::tests::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    SessionFixture fixture{
        io.get_executor(),
        policies(R"({"allow":[{"tool_pattern":"AgentRun"}],"deny":[{"tool_pattern":"MemoryRecall"}]})",
                 R"({"allow":[{"tool_pattern":"MemoryRecall"}]})")};
    memory::longterm::Record note;
    note.key = {.id = "private", .scope_key = "scope-A"};
    note.title = "Private note";
    note.body = "PARENT_PRIVATE_MEMORY";
    auto stored = co_await fixture.assembly.longterm_memory_backend()->upsert({.record = note});
    REQUIRE(stored.has_value());
    ScriptedProvider provider{{
        calls({call("child", "AgentRun", R"({"agent":"worker","prompt":"Review the task"})")}),
        answer("reviewed the available context"),
        answer("done"),
    }};
    auto session = bootstrap::AgentSession::create(fixture.options(provider));
    REQUIRE(session.has_value());
    auto result = co_await (*session)->run_prompt({.prompt = "Delegate the review"});
    REQUIRE(result.has_value());
    REQUIRE(provider.requests.size() == 3);
    for (const auto& request : provider.requests) {
      REQUIRE(request.system_prompt.has_value());
      CHECK(request.system_prompt->contains("Memory index unavailable (permission_denied)"));
      CHECK_FALSE(request.system_prompt->contains("PARENT_PRIVATE_MEMORY"));
    }
    auto after = co_await fixture.assembly.longterm_memory_backend()->get(note.key);
    REQUIRE(after.has_value());
    CHECK(*after == note);
  });
}

TEST_CASE("AgentRun child writes obey both parent and child policy", "[integration][bootstrap][collaboration]") {
  std::string parent;
  std::string child = R"({"allow":[{"tool_pattern":"FileWrite"}]})";
  bool requires_approval = false;
  SECTION("parent explicit deny") {
    parent = R"({"allow":[{"tool_pattern":"AgentRun"}],"deny":[{"tool_pattern":"FileWrite"}]})";
  }
  SECTION("parent requires approval") {
    parent = R"({"allow":[{"tool_pattern":"AgentRun"}],"ask":[{"tool_pattern":"FileWrite"}]})";
    requires_approval = true;
  }
  SECTION("parent strict default denies the child's grant") {
    parent = R"({"allow":[{"tool_pattern":"AgentRun"}]})";
  }
  SECTION("child restricts the parent's grant") {
    parent = R"({"allow":[{"tool_pattern":"AgentRun"},{"tool_pattern":"FileWrite"}]})";
    child = R"({"deny":[{"tool_pattern":"FileWrite"}]})";
  }
  orangutan::tests::run_async([&parent, &child, requires_approval](asio::io_context& io) -> async::Awaitable<void> {
    SessionFixture fixture{io.get_executor(), policies(parent, child)};
    ScriptedProvider provider{{
        calls({call("child-1", "AgentRun", R"({"agent":"worker","prompt":"write"})")}),
        calls({call("write-1", "FileWrite", R"({"path":"blocked.txt","content":"unauthorized"})")}),
        answer("write refused"),
        answer("parent done"),
    }};
    auto session = bootstrap::AgentSession::create(fixture.options(provider));
    REQUIRE(session.has_value());

    auto result = co_await (*session)->run_prompt({.prompt = "delegate"});

    REQUIRE_FALSE(std::filesystem::exists(fixture.workspace.path / "blocked.txt"));
    REQUIRE(result.has_value());
    REQUIRE(provider.requests.size() == 4);
    const auto& refused = result_in(provider.requests[2], "write-1");
    REQUIRE(refused.is_error);
    REQUIRE(refused.output.contains(requires_approval ? "tool requires approval" : "tool denied by permission rules"));
    REQUIRE(refused.output.contains("approval_required") == requires_approval);
  });
}

TEST_CASE("AgentRun refuses spawning without the parent's capability", "[integration][bootstrap][collaboration]") {
  orangutan::tests::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    SessionFixture fixture{
        io.get_executor(),
        policies(R"({"allow":[{"tool_pattern":"*"}],"deny":[{"tool_pattern":"*","capability":"spawn_agent"}]})",
                 R"({"allow":[{"tool_pattern":"*"}]})")};
    ScriptedProvider provider{{calls({call("child-1", "AgentRun", R"({"agent":"worker","prompt":"inspect"})")}),
                               answer("done"),
                               answer("done")}};
    auto session = bootstrap::AgentSession::create(fixture.options(provider));
    REQUIRE(session.has_value());

    auto result = co_await (*session)->run_prompt({.prompt = "delegate"});

    REQUIRE(provider.requests.size() == 2);
    REQUIRE(result.has_value());
    REQUIRE(result_in(provider.requests[1], "child-1").is_error);
    REQUIRE(fixture.worker_session_count() == 0);
  });
}

TEST_CASE("AgentRun evaluates parent restrictions after a child input rewrite",
          "[integration][bootstrap][collaboration]") {
  orangutan::tests::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    SessionFixture fixture{
        io.get_executor(),
        policies(
            R"({"allow":[{"tool_pattern":"AgentRun"},{"tool_pattern":"FileWrite","input_pattern":"allowed.txt"}]})",
            R"({"allow":[{"tool_pattern":"FileWrite"}]})")};
    hook::InProcessSink rewrite{
        "rewrite-child",
        [](hook::Event, hook::PayloadPtr) -> async::Awaitable<core::Result<void>> { co_return core::Result<void>{}; }};
    rewrite.set_blocking_handler(
        [](hook::Event, hook::PayloadPtr payload) -> async::Awaitable<core::Result<hook::HookDecision>> {
          const auto& before = std::get<hook::ToolBeforePayload>(*payload);
          hook::HookDecision decision;
          if (before.who.agent_key == "worker" && before.tool_name == "FileWrite") {
            decision.kind = hook::HookDecisionKind::rewrite;
            decision.rewritten_input_json = R"({"path":"blocked.txt","content":"rewritten"})";
          }
          co_return decision;
        });
    fixture.assembly.hook_bus().bind(rewrite, {hook::Event::tool_before});
    ScriptedProvider provider{{
        calls({call("child-1", "AgentRun", R"({"agent":"worker","prompt":"write"})")}),
        calls({call("write-1", "FileWrite", R"({"path":"allowed.txt","content":"original"})")}),
        answer("write refused"),
        answer("done"),
    }};
    auto session = bootstrap::AgentSession::create(fixture.options(provider));
    REQUIRE(session.has_value());

    auto result = co_await (*session)->run_prompt({.prompt = "delegate"});

    REQUIRE_FALSE(std::filesystem::exists(fixture.workspace.path / "blocked.txt"));
    REQUIRE_FALSE(std::filesystem::exists(fixture.workspace.path / "allowed.txt"));
    REQUIRE(result.has_value());
    REQUIRE(provider.requests.size() == 4);
    REQUIRE(result_in(provider.requests[2], "write-1").is_error);
  });
}

TEST_CASE("AgentRun admits a bounded number of children in each prompt", "[integration][bootstrap][collaboration]") {
  orangutan::tests::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    SessionFixture fixture{io.get_executor()};
    const auto batch = calls({call("first", "AgentRun", R"({"agent":"worker","prompt":"first task"})"),
                              call("second", "AgentRun", R"({"agent":"worker","prompt":"second task"})")});
    ScriptedProvider provider{{batch,
                               answer("child answer"),
                               answer("parent answer"),
                               batch,
                               answer("child answer"),
                               answer("parent answer")}};
    auto options = fixture.options(provider);
    options.max_child_runs = 1;
    auto session = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(session.has_value());

    for (std::size_t prompt = 1; prompt <= 2; ++prompt) {
      auto result = co_await (*session)->run_prompt({.prompt = "delegate twice"});

      REQUIRE(fixture.worker_session_count() == static_cast<std::int64_t>(prompt));
      REQUIRE(result.has_value());
      REQUIRE(provider.requests.size() == prompt * 3);
      const auto& results = provider.requests.back().messages.back().blocks;
      REQUIRE(results.size() == 2);
      REQUIRE(std::ranges::count_if(results, [](const auto& block) {
                return std::get<core::ToolResultContent>(block).is_error;
              }) == 1);
      REQUIRE(std::ranges::any_of(results, [](const auto& block) {
        return std::get<core::ToolResultContent>(block).output.contains("child_limit");
      }));
    }
  });
}

TEST_CASE("AgentRun children cannot delegate another generation", "[integration][bootstrap][collaboration]") {
  orangutan::tests::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    SessionFixture fixture{io.get_executor()};
    const auto delegate = calls({call("child-1", "AgentRun", R"({"agent":"worker","prompt":"delegate again"})")});
    ScriptedProvider provider{{delegate, delegate, answer("done"), answer("done"), answer("done")}};
    auto session = bootstrap::AgentSession::create(fixture.options(provider));
    REQUIRE(session.has_value());

    auto result = co_await (*session)->run_prompt({.prompt = "delegate"});

    REQUIRE(provider.requests.size() == 4);
    REQUIRE(result.has_value());
    const auto& refused = result_in(provider.requests[2], "child-1");
    REQUIRE(refused.is_error);
    REQUIRE(refused.output.contains("delegation_disabled"));
    REQUIRE(fixture.worker_session_count() == 1);
  });
}

TEST_CASE("AgentRun cancellation joins its child without waiting for another session",
          "[integration][bootstrap][collaboration][cancellation]") {
  orangutan::tests::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        using namespace asio::experimental::awaitable_operators;
        SessionFixture fixture{io.get_executor()};
        async::Channel<std::string> started{io.get_executor(), 2};
        async::Channel<int> release_child{io.get_executor(), 1};
        async::Channel<int> release_independent{io.get_executor(), 1};
        async::Channel<int> parent_done{io.get_executor(), 1};
        async::Channel<int> independent_done{io.get_executor(), 1};
        bool child_finished = false;
        bool independent_finished = false;
        tool::Registry registry;
        const auto names = std::array{std::string{"worker"}};
        REQUIRE(tool::register_agent_run(registry, names));
        auto definition = core::ToolDef::with_no_input("CleanupGate", "Hold a tool until cleanup is released");
        REQUIRE(registry.add(std::move(definition),
                             [&started, &release_child, &release_independent, &child_finished, &independent_finished](
                                 std::string_view,
                                 tool::DispatchContext& context) -> async::Awaitable<core::Result<tool::Output>> {
                               co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
                               auto signalled = started.try_send(context.agent_key);
                               if (!signalled)
                                 co_return std::unexpected(std::move(signalled).error());
                               const bool child = context.agent_key == "worker";
                               auto released = co_await (child ? release_child : release_independent).receive();
                               if (!released)
                                 co_return std::unexpected(std::move(released).error());
                               (child ? child_finished : independent_finished) = true;
                               co_return tool::Output::text_only("cleanup complete");
                             }));
        agent::ToolScheduler scheduler{io.get_executor(), registry};
        ScriptedProvider parent_provider{{
            calls({call("child-1", "AgentRun", R"({"agent":"worker","prompt":"work"})")}),
            calls({call("cleanup", "CleanupGate", "{}")}),
            answer("child done"),
            answer("parent done"),
        }};
        ScriptedProvider independent_provider{
            {calls({call("cleanup", "CleanupGate", "{}")}), answer("independent done")}};
        auto options = fixture.options(parent_provider);
        options.registry = &registry;
        options.scheduler = &scheduler;
        auto parent = bootstrap::AgentSession::create(options);
        REQUIRE(parent.has_value());
        options.provider = &independent_provider;
        options.agent_key = "independent";
        options.session_id.back() = std::byte{0x43};
        options.max_child_runs = 0;
        auto independent = bootstrap::AgentSession::create(std::move(options));
        REQUIRE(independent.has_value());
        asio::cancellation_signal cancellation;
        std::optional<core::Result<agent::PromptResult>> parent_result;
        std::optional<core::Result<agent::PromptResult>> independent_result;
        std::exception_ptr parent_failure;
        std::exception_ptr independent_failure;
        asio::co_spawn(io,
                       (*parent)->run_prompt({.prompt = "delegate"}),
                       asio::bind_cancellation_slot(
                           cancellation.slot(),
                           [&parent_result, &parent_failure, &parent_done](std::exception_ptr failure,
                                                                           core::Result<agent::PromptResult> result) {
                             parent_failure = failure;
                             parent_result = std::move(result);
                             static_cast<void>(parent_done.try_send(0));
                           }));
        auto child_started = co_await started.receive();
        REQUIRE(child_started.has_value());
        REQUIRE(*child_started == "worker");
        asio::co_spawn(io,
                       (*independent)->run_prompt({.prompt = "independent"}),
                       [&independent_result,
                        &independent_failure,
                        &independent_done](std::exception_ptr failure, core::Result<agent::PromptResult> result) {
                         independent_failure = failure;
                         independent_result = std::move(result);
                         static_cast<void>(independent_done.try_send(0));
                       });
        auto independent_started = co_await started.receive();
        REQUIRE(independent_started.has_value());
        REQUIRE(*independent_started == "independent");

        cancellation.emit(asio::cancellation_type::all);
        // The observation deadline exceeds the scheduler's 100 ms cancellation grace.
        auto early = co_await (parent_done.receive() || async::sleep_for(io.get_executor(), 250ms));
        const bool returned_before_cleanup = std::holds_alternative<core::Result<int>>(early);
        const bool child_finished_before_release = child_finished;
        REQUIRE(release_child.try_send(0));
        bool returned_before_independent = returned_before_cleanup;
        if (!returned_before_cleanup) {
          auto joined = co_await (parent_done.receive() || async::sleep_for(io.get_executor(), 500ms));
          returned_before_independent = std::holds_alternative<core::Result<int>>(joined);
        }
        const bool independent_finished_before_release = independent_finished;
        REQUIRE(release_independent.try_send(0));
        if (!returned_before_independent) {
          auto done = co_await parent_done.receive();
          REQUIRE(done.has_value());
        }
        auto done = co_await independent_done.receive();
        REQUIRE(done.has_value());
        auto drained = co_await scheduler.wait_idle();
        REQUIRE(drained.has_value());

        REQUIRE_FALSE(returned_before_cleanup);
        REQUIRE_FALSE(child_finished_before_release);
        REQUIRE(returned_before_independent);
        REQUIRE_FALSE(independent_finished_before_release);
        REQUIRE(child_finished);
        REQUIRE(independent_finished);
        REQUIRE_FALSE(parent_failure);
        REQUIRE_FALSE(independent_failure);
        REQUIRE(parent_result.has_value());
        REQUIRE_FALSE(parent_result->has_value());
        REQUIRE(parent_result->error().kind() == core::ErrorKind::cancelled);
        REQUIRE(independent_result.has_value());
        REQUIRE(independent_result->has_value());
        REQUIRE((*independent_result)->text == "independent done");
        REQUIRE(fixture.worker_session_count() == 0);
      },
      5s);
}

TEST_CASE("AgentRun child writes proceed after bounded approval", "[integration][bootstrap][collaboration]") {
  orangutan::tests::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    SessionFixture fixture{
        io.get_executor(),
        policies(
            R"({"allow":[{"tool_pattern":"AgentRun"}],"ask":[{"tool_pattern":"FileWrite","replay_max":2,"approval_ttl_seconds":90}]})",
            R"({"ask":[{"tool_pattern":"FileWrite","replay_max":8,"approval_ttl_seconds":20}]})")};
    std::vector<hook::PermissionAskRenderedPayload> approvals;
    hook::InProcessSink approve{
        "approve-child",
        [](hook::Event, hook::PayloadPtr) -> async::Awaitable<core::Result<void>> { co_return core::Result<void>{}; }};
    approve.set_blocking_handler(
        [&approvals](hook::Event, hook::PayloadPtr payload) -> async::Awaitable<core::Result<hook::HookDecision>> {
          approvals.push_back(std::get<hook::PermissionAskRenderedPayload>(*payload));
          hook::HookDecision decision;
          decision.reason = "operator_approved:owner";
          co_return decision;
        });
    fixture.assembly.hook_bus().bind(approve, {hook::Event::permission_ask_rendered});
    ScriptedProvider provider{{
        calls({call("child-1", "AgentRun", R"({"agent":"worker","prompt":"write"})")}),
        calls({call("write-1", "FileWrite", R"({"path":"approved.txt","content":"approved child content"})")}),
        answer("child wrote the file"),
        answer("done"),
    }};
    auto session = bootstrap::AgentSession::create(fixture.options(provider));
    REQUIRE(session.has_value());

    auto result = co_await (*session)->run_prompt({.prompt = "delegate"});

    REQUIRE(result.has_value());
    REQUIRE(approvals.size() == 1);
    REQUIRE(approvals[0].who.agent_key == "worker");
    REQUIRE(approvals[0].who.identity != "owner");
    REQUIRE(approvals[0].replay_max == 2);
    REQUIRE(approvals[0].approval_ttl == 20s);
    auto file = std::ifstream{fixture.workspace.path / "approved.txt"};
    REQUIRE(file.is_open());
    const auto content = std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    REQUIRE(content == "approved child content");
    REQUIRE(provider.requests.size() == 4);
    REQUIRE_FALSE(result_in(provider.requests[2], "write-1").is_error);
  });
}
