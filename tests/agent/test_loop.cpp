// tests/agent/test_loop.cpp — first fake-provider-backed Loop coverage.
//
// These tests intentionally stay inside spec 0017's fake-provider-first loop
// envelope: prompt/request construction, direct sequential tool dispatch,
// model-visible repair errors, terminal-success trace rows, cancellation trace
// rows, and loop boundary errors. Provider retry/fallback, non-cancellation
// error trace rows, and the scheduler remain later slices.

#include <oran/agent.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/post.hpp>
#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/config.hpp>
#include <oran/core/content.hpp>
#include <oran/core/error.hpp>
#include <oran/core/message.hpp>
#include <oran/core/role.hpp>
#include <oran/core/stop_reason.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/permission.hpp>
#include <oran/prompt.hpp>
#include <oran/provider.hpp>
#include <oran/storage.hpp>
#include <oran/tool.hpp>

#include "../test-helpers/run_async.hpp"

namespace agent = orangutan::agent;
namespace async = orangutan::async;
namespace config = orangutan::config;
namespace core = orangutan::core;
namespace permission = orangutan::permission;
namespace prompt = orangutan::prompt;
namespace provider = orangutan::provider;
namespace storage = orangutan::storage;
namespace test = orangutan::tests;
namespace tool = orangutan::tool;

namespace {

using namespace std::chrono_literals;

provider::Route default_route(provider::PromptCacheOptions cache = {}) {
  return provider::Route{
      .primary =
          provider::ModelTarget{
              .profile = "fake",
              .model = "fake-1",
              .protocol = provider::ProtocolKind::anthropic_messages,
              .thinking_budget = std::nullopt,
              .cache = cache,
          },
      .fallbacks = {},
  };
}

core::ToolDef tool_def(std::string name, std::string description, bool deferred = false) {
  return core::ToolDef{
      .name = std::move(name),
      .description = std::move(description),
      .input_schema_json = R"({"type":"object","properties":{},"additionalProperties":false})",
      .required_capabilities = {},
      .deferred = deferred,
      .category = "test",
  };
}

core::ToolDef canned_tool_def(std::string name) {
  auto def = tool_def(std::move(name), "Return a canned result");
  def.input_schema_json = R"({"type":"object","properties":{"value":{"type":"string"}},"additionalProperties":false})";
  return def;
}

void add_canned_tool(tool::Registry& registry,
                     core::ToolDef def,
                     std::string output,
                     bool output_error = false,
                     std::optional<core::ErrorKind> error_kind = std::nullopt) {
  auto handler = [output = std::move(output), output_error, error_kind](
                     std::string_view input,
                     tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
    if (error_kind.has_value()) {
      co_return std::unexpected(core::Error{*error_kind, output});
    }
    const auto text = output + ":" + std::string{input};
    if (output_error) {
      co_return tool::Output::error(text);
    }
    co_return tool::Output::text_only(text);
  };
  REQUIRE(registry.add(std::move(def), std::move(handler)).has_value());
}

const core::ToolResultContent& tool_result_at(const core::Message& message, std::size_t index) {
  REQUIRE(index < message.blocks.size());
  const auto* result = std::get_if<core::ToolResultContent>(&message.blocks[index]);
  REQUIRE(result != nullptr);
  return *result;
}

const core::ToolUseContent& tool_use_at(const core::Message& message, std::size_t index) {
  REQUIRE(index < message.blocks.size());
  const auto* result = std::get_if<core::ToolUseContent>(&message.blocks[index]);
  REQUIRE(result != nullptr);
  return *result;
}

bool contains_context(const core::Error& error, std::string_view key, std::string_view value) {
  return std::ranges::any_of(error.context(),
                             [&](const auto& entry) { return entry.first == key && entry.second == value; });
}

std::string tool_result_output_in(const provider::Request& request, std::string_view tool_use_id) {
  for (const auto& message : request.messages) {
    for (const auto& block : message.blocks) {
      if (const auto* result = std::get_if<core::ToolResultContent>(&block);
          result != nullptr && result->tool_use_id == tool_use_id) {
        return result->output;
      }
    }
  }
  return {};
}

permission::RuleSet allow_all_rules() {
  permission::RuleSet rules;
  rules.add(permission::Rule{
      .verdict = permission::Verdict::allow,
      .tool_pattern = "*",
      .capability = std::nullopt,
  });
  return rules;
}

core::TurnId turn_id_with(unsigned char seed) {
  core::TurnId id{};
  for (std::size_t i = 0; i < id.size(); ++i) {
    id[i] = static_cast<std::byte>(seed + i);
  }
  return id;
}

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

storage::Pool open_trace_pool(asio::io_context& io, TempDb& db) {
  auto pool =
      storage::Pool::open(io.get_executor(),
                          storage::PoolOptions{.path = db.string(), .reader_count = 2, .statement_cache_capacity = 8});
  REQUIRE(pool.has_value());
  return std::move(*pool);
}

std::uint64_t prompt_section_hash(const prompt::RenderedPrompt& rendered, std::string_view id) {
  const auto it = std::ranges::find(rendered.sections, id, &prompt::CacheSection::id);
  REQUIRE(it != rendered.sections.end());
  return it->content_hash;
}

tool::DispatchContext dispatch_context(asio::io_context& io,
                                       permission::RuleSet& rules,
                                       permission::AuditSink& audit,
                                       std::string identity = "operator-1") {
  return tool::DispatchContext{
      .executor = io.get_executor(),
      .mode = permission::Mode::strict,
      .rules = rules,
      .audit = audit,
      .scope_key = "scope-A",
      .agent_key = "coder",
      .identity = std::move(identity),
  };
}

std::vector<core::ToolDef> loop_catalog() {
  return {
      tool_def("memory.recall", "Recall memory", true),
      canned_tool_def("file.read"),
      tool_def("custom.non_default", "A registered tool outside the default prompt set"),
  };
}

agent::RunTurnInputs base_inputs(const std::vector<core::ToolDef>& catalog, const std::vector<core::Message>& tail) {
  return agent::RunTurnInputs{
      .system_preamble = "system: deterministic test preamble",
      .tool_catalog = catalog,
      .active_tools = config::PromptActiveToolsConfig{},
      .promoted_tools = {},
      .skills_catalog = "skills: none",
      .memory_framing = "memory: none",
      .per_agent_overlay = "overlay: coder",
      .conversation_tail = tail,
      .tool_choice = std::string{"auto"},
      .max_tokens = 512,
      .thinking_budget = std::nullopt,
      .retry = provider::RetryPolicy{},
      .stream = true,
  };
}

/// RecordingProvider is a narrow white-box fixture for Loop's request mapping.
/// FakeProvider proves the scripted provider contract; this fixture proves the
/// loop sends the exact domain request future real adapters will consume.
class RecordingProvider final : public provider::System {
public:
  explicit RecordingProvider(provider::Response response) : response_{std::move(response)} {}

  [[nodiscard]] async::Awaitable<core::Result<provider::Response>>
  send(provider::Request request, provider::Route route, provider::EventSink* sink = nullptr) const override {
    request_ = std::move(request);
    route_ = std::move(route);
    ++calls_;
    if (sink != nullptr) {
      sink->on_done(response_.stop_reason);
    }
    co_return response_;
  }

  [[nodiscard]] const std::optional<provider::Request>& request() const noexcept {
    return request_;
  }

  [[nodiscard]] const std::optional<provider::Route>& route() const noexcept {
    return route_;
  }

  [[nodiscard]] std::size_t calls() const noexcept {
    return calls_;
  }

private:
  provider::Response response_;
  mutable std::optional<provider::Request> request_;
  mutable std::optional<provider::Route> route_;
  mutable std::size_t calls_{0};
};

class RecordingSequenceProvider final : public provider::System {
public:
  explicit RecordingSequenceProvider(std::vector<provider::Response> responses) : responses_{std::move(responses)} {}

  [[nodiscard]] async::Awaitable<core::Result<provider::Response>>
  send(provider::Request request, provider::Route route, provider::EventSink* sink = nullptr) const override {
    requests_.push_back(std::move(request));
    routes_.push_back(std::move(route));
    const auto index = cursor_++;
    if (index >= responses_.size()) {
      co_return std::unexpected(core::Error::internal("provider plan exhausted"));
    }
    if (sink != nullptr) {
      sink->on_done(responses_[index].stop_reason);
    }
    co_return responses_[index];
  }

  [[nodiscard]] std::span<const provider::Request> requests() const noexcept {
    return requests_;
  }

private:
  std::vector<provider::Response> responses_;
  mutable std::vector<provider::Request> requests_;
  mutable std::vector<provider::Route> routes_;
  mutable std::size_t cursor_{0};
};

}  // namespace

TEST_CASE("Loop returns text from a single fake-provider end_turn", "[unit][agent][loop]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response =
            provider::Response{
                .blocks = {core::TextContent{.text = "done"}},
                .stop_reason = core::StopReason::end_turn,
                .usage = provider::Usage{.input_tokens = 7,
                                         .output_tokens = 2,
                                         .cache_creation_tokens = 0,
                                         .cache_read_tokens = 0,
                                         .cost_estimate = std::nullopt},
                .model_used = std::string{"fake-1"},
            },
        .deltas = {},
        .error = std::nullopt,
        .latency = {},
    });
    provider::FakeProvider fake{std::move(plan)};
    agent::Loop loop{fake, default_route()};

    const auto catalog = loop_catalog();
    const std::vector<core::Message> tail{core::Message::user_text("hello")};
    auto result = co_await loop.run_turn(base_inputs(catalog, tail));

    REQUIRE(result.has_value());
    REQUIRE(result->text == "done");
    REQUIRE(result->stop_reason == core::StopReason::end_turn);
    REQUIRE(result->usage.input_tokens == 7);
    REQUIRE(result->usage.output_tokens == 2);
    REQUIRE(result->model_used == std::string{"fake-1"});
    REQUIRE(result->iterations == 1);
    REQUIRE(result->assistant_blocks.size() == 1);
    REQUIRE(result->rendered_prompt.sections.size() == 7);
    REQUIRE(result->cache_hints.has_value());
    REQUIRE(result->cache_hints->prefix_sections.size() == 6);
    REQUIRE(fake.turns_consumed() == 1);
  });
}

TEST_CASE("Loop maps prompt, messages, active tools, and cache hints into the provider request",
          "[unit][agent][loop]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    RecordingProvider provider{provider::Response{
        .blocks = {core::TextContent{.text = "mapped"}},
        .stop_reason = core::StopReason::end_turn,
        .usage = {},
        .model_used = std::nullopt,
    }};
    agent::Loop loop{provider, default_route(provider::PromptCacheOptions{.enabled = true, .min_prefix_bytes = 1})};

    const auto catalog = loop_catalog();
    const std::vector<std::string> promoted{"memory.recall"};
    const std::vector<core::Message> tail{core::Message::user_text("map this")};
    auto inputs = base_inputs(catalog, tail);
    inputs.promoted_tools = promoted;
    auto result = co_await loop.run_turn(inputs);

    REQUIRE(result.has_value());
    REQUIRE(provider.calls() == 1);
    REQUIRE(provider.request().has_value());

    const auto& request = *provider.request();
    REQUIRE(request.messages == tail);
    REQUIRE(request.stream);
    REQUIRE(request.tool_choice == std::string{"auto"});
    REQUIRE(request.max_tokens == 512);
    REQUIRE(request.system_prompt.has_value());
    REQUIRE(request.system_prompt->contains("system: deterministic test preamble"));
    REQUIRE(request.system_prompt->contains("Tool: file.read"));
    REQUIRE(request.system_prompt->contains("Tool: memory.recall"));
    REQUIRE_FALSE(request.system_prompt->contains("Tool: custom.non_default"));

    REQUIRE(request.tools.size() == 2);
    REQUIRE(request.tools[0].name == "file.read");
    REQUIRE(request.tools[1].name == "memory.recall");
    REQUIRE(request.cache.has_value());
    REQUIRE(request.cache->prefix_bytes == result->rendered_prompt.prefix_bytes);
    REQUIRE(provider.route().has_value());
    REQUIRE(provider.route()->primary.model == "fake-1");
  });
}

TEST_CASE("Loop forwards provider errors unchanged", "[unit][agent][loop]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response = std::nullopt,
        .deltas = {},
        .error = core::Error::network("upstream timeout"),
        .latency = {},
    });
    provider::FakeProvider fake{std::move(plan)};
    agent::Loop loop{fake, default_route()};

    const auto catalog = loop_catalog();
    const std::vector<core::Message> tail{core::Message::user_text("hello")};
    auto result = co_await loop.run_turn(base_inputs(catalog, tail));

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::network);
    REQUIRE(result.error().retryable());
  });
}

TEST_CASE("Loop annotates cancellation during provider await", "[unit][agent][loop][cancellation]") {
  asio::io_context io;
  asio::cancellation_signal signal;

  std::vector<provider::ScriptedTurn> plan;
  plan.push_back(provider::ScriptedTurn{
      .response =
          provider::Response{
              .blocks = {core::TextContent{.text = "late"}},
              .stop_reason = core::StopReason::end_turn,
              .usage = {},
              .model_used = std::nullopt,
          },
      .deltas = {},
      .error = std::nullopt,
      .latency = 1s,
  });
  provider::FakeProvider fake{std::move(plan)};
  agent::Loop loop{fake, default_route()};

  const auto catalog = loop_catalog();
  const std::vector<core::Message> tail{core::Message::user_text("cancel provider")};
  std::optional<core::Result<agent::RunTurnResult>> result;
  std::exception_ptr failure;

  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<agent::RunTurnResult>> {
        co_return co_await loop.run_turn(base_inputs(catalog, tail));
      },
      asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, core::Result<agent::RunTurnResult> r) {
        failure = ep;
        result = std::move(r);
        io.stop();
      }));

  asio::post(io, [&] { signal.emit(asio::cancellation_type::terminal); });
  io.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
  REQUIRE(contains_context(result->error(), "reason", "parent_cancelled"));
  REQUIRE(contains_context(result->error(), "cancellation_phase", "provider"));
  REQUIRE(fake.turns_consumed() == 1);
}

TEST_CASE("Loop rejects tool-use responses until the dispatch iteration slice lands", "[unit][agent][loop]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response =
            provider::Response{
                .blocks = {core::ToolUseContent{.id = "t1", .name = "file.read", .input_json = R"({"path":"x"})"}},
                .stop_reason = core::StopReason::tool_use,
                .usage = {},
                .model_used = std::nullopt,
            },
        .deltas = {},
        .error = std::nullopt,
        .latency = {},
    });
    provider::FakeProvider fake{std::move(plan)};
    agent::Loop loop{fake, default_route()};

    const auto catalog = loop_catalog();
    const std::vector<core::Message> tail{core::Message::user_text("read x")};
    auto result = co_await loop.run_turn(base_inputs(catalog, tail));

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::internal);
    REQUIRE(result.error().message() == "agent loop: response requires a later loop slice");
  });
}

TEST_CASE("Loop dispatches one tool_use and re-enters the provider with a tool result", "[unit][agent][loop]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    RecordingSequenceProvider provider{std::vector<provider::Response>{
        provider::Response{
            .blocks = {core::ToolUseContent{.id = "t1", .name = "file.read", .input_json = R"({"value":"a"})"}},
            .stop_reason = core::StopReason::tool_use,
            .usage = provider::Usage{.input_tokens = 5,
                                     .output_tokens = 1,
                                     .cache_creation_tokens = 0,
                                     .cache_read_tokens = 0,
                                     .cost_estimate = std::nullopt},
            .model_used = std::nullopt,
        },
        provider::Response{
            .blocks = {core::TextContent{.text = "final"}},
            .stop_reason = core::StopReason::end_turn,
            .usage = provider::Usage{.input_tokens = 3,
                                     .output_tokens = 2,
                                     .cache_creation_tokens = 0,
                                     .cache_read_tokens = 0,
                                     .cost_estimate = std::nullopt},
            .model_used = std::string{"fake-1"},
        },
    }};
    agent::Loop loop{provider, default_route()};
    tool::Registry registry;
    add_canned_tool(registry, canned_tool_def("file.read"), "read-ok");
    auto rules = allow_all_rules();
    permission::RecordingAuditSink audit;
    auto ctx = dispatch_context(io, rules, audit);
    ctx.parent_turn_id = turn_id_with(0x70);

    const auto catalog = registry.catalog();
    const std::vector<core::Message> tail{core::Message::user_text("read")};
    auto inputs = base_inputs(catalog, tail);
    inputs.turn_id = turn_id_with(0x20);
    inputs.tools = &registry;
    inputs.dispatch_context = &ctx;
    auto result = co_await loop.run_turn(inputs);

    REQUIRE(result.has_value());
    REQUIRE(result->text == "final");
    REQUIRE(result->iterations == 2);
    REQUIRE(result->usage.input_tokens == 8);
    REQUIRE(result->usage.output_tokens == 3);
    REQUIRE(result->model_used == std::string{"fake-1"});
    REQUIRE(provider.requests().size() == 2);
    REQUIRE(provider.requests()[0].messages == tail);
    REQUIRE(provider.requests()[1].messages.size() == 3);
    REQUIRE(provider.requests()[1].messages[0] == tail[0]);
    REQUIRE(provider.requests()[1].messages[1].role == core::Role::assistant);
    REQUIRE(tool_use_at(provider.requests()[1].messages[1], 0).id == "t1");
    REQUIRE(provider.requests()[1].messages[2].role == core::Role::tool);
    const auto& tool_result = tool_result_at(provider.requests()[1].messages[2], 0);
    REQUIRE(tool_result.tool_use_id == "t1");
    REQUIRE(tool_result.output == R"(read-ok:{"value":"a"})");
    REQUIRE_FALSE(tool_result.is_error);
    REQUIRE(tool_result_output_in(provider.requests()[1], "t1") == R"(read-ok:{"value":"a"})");
    REQUIRE(result->transcript.size() == 4);
    REQUIRE(result->transcript[0] == tail[0]);
    REQUIRE(result->transcript[1] == provider.requests()[1].messages[1]);
    REQUIRE(result->transcript[2] == provider.requests()[1].messages[2]);
    REQUIRE(result->transcript[3].role == core::Role::assistant);
    REQUIRE(std::get<core::TextContent>(result->transcript[3].blocks[0]).text == "final");
    REQUIRE(audit.events().size() == 1);
    REQUIRE(audit.events()[0].tool_name == "file.read");
    REQUIRE(audit.events()[0].parent_turn_id.has_value());
    REQUIRE(*audit.events()[0].parent_turn_id == turn_id_with(0x20));
    REQUIRE(ctx.parent_turn_id.has_value());
    REQUIRE(*ctx.parent_turn_id == turn_id_with(0x70));
  });
}

TEST_CASE("Loop persists one terminal trace row for a text turn", "[unit][agent][loop][trace]") {
  TempDb db{"oran-agent-loop-trace-text"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_trace_pool(io, db);
    storage::TraceRepository trace{pool};
    auto migrated = co_await trace.migrate();
    REQUIRE(migrated.has_value());

    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response =
            provider::Response{
                .blocks = {core::TextContent{.text = "done"}},
                .stop_reason = core::StopReason::end_turn,
                .usage = provider::Usage{.input_tokens = 11,
                                         .output_tokens = 5,
                                         .cache_creation_tokens = 2,
                                         .cache_read_tokens = 7,
                                         .cost_estimate = 0.012},
                .model_used = std::string{"fake-trace-model"},
            },
        .deltas = {},
        .error = std::nullopt,
        .latency = {},
    });
    provider::FakeProvider fake{std::move(plan)};
    agent::Loop loop{fake, default_route()};

    const auto catalog = loop_catalog();
    const std::vector<core::Message> tail{core::Message::user_text("trace me")};
    auto inputs = base_inputs(catalog, tail);
    inputs.turn_id = turn_id_with(0x31);
    inputs.trace = agent::TraceContext{
        .repository = &trace,
        .session_id = turn_id_with(0x80),
        .parent_turn_id = turn_id_with(0x22),
        .agent_key = "coder",
        .origin = "cli",
        .context_json = R"json({"source":"test"})json",
    };
    auto result = co_await loop.run_turn(inputs);

    REQUIRE(result.has_value());
    REQUIRE(result->text == "done");

    auto row = co_await trace.get_turn(turn_id_with(0x31));
    REQUIRE(row.has_value());
    REQUIRE(row->has_value());
    REQUIRE((*row)->turn_id == turn_id_with(0x31));
    REQUIRE((*row)->parent_turn_id.has_value());
    REQUIRE(*(*row)->parent_turn_id == turn_id_with(0x22));
    REQUIRE((*row)->session_id == turn_id_with(0x80));
    REQUIRE((*row)->agent_key == "coder");
    REQUIRE((*row)->origin == "cli");
    REQUIRE((*row)->route_profile == "fake");
    REQUIRE((*row)->route_model == "fake-trace-model");
    REQUIRE((*row)->started_at_ns > 0);
    REQUIRE((*row)->finished_at_ns >= (*row)->started_at_ns);
    REQUIRE((*row)->stop_reason == "end_turn");
    REQUIRE((*row)->iteration_count == 1);
    REQUIRE((*row)->prompt_prefix_hash == result->rendered_prompt.prefix_hash);
    REQUIRE((*row)->prompt_prefix_bytes == static_cast<std::int64_t>(result->rendered_prompt.prefix_bytes));
    REQUIRE((*row)->active_catalog_hash == prompt_section_hash(result->rendered_prompt, "tool_catalog"));
    REQUIRE((*row)->deferred_catalog_hash == prompt_section_hash(result->rendered_prompt, "deferred_tools"));
    REQUIRE((*row)->cache_creation_tokens == 2);
    REQUIRE((*row)->cache_read_tokens == 7);
    REQUIRE((*row)->input_tokens == 11);
    REQUIRE((*row)->output_tokens == 5);
    REQUIRE((*row)->cost_estimate_usd == 0.012);
    REQUIRE_FALSE((*row)->cancellation_phase.has_value());
    REQUIRE((*row)->context_json == R"json({"source":"test"})json");
  });
}

TEST_CASE("Loop persists a terminal trace row and correlates storage audit rows", "[unit][agent][loop][trace]") {
  TempDb db{"oran-agent-loop-trace-audit"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_trace_pool(io, db);
    storage::TraceRepository trace{pool};
    storage::AuditRepository audit_repo{pool};
    auto migrated = co_await trace.migrate();
    REQUIRE(migrated.has_value());

    RecordingSequenceProvider provider{std::vector<provider::Response>{
        provider::Response{
            .blocks = {core::ToolUseContent{.id = "t1", .name = "file.read", .input_json = R"({"value":"a"})"}},
            .stop_reason = core::StopReason::tool_use,
            .usage = provider::Usage{.input_tokens = 11,
                                     .output_tokens = 1,
                                     .cache_creation_tokens = 2,
                                     .cache_read_tokens = 7,
                                     .cost_estimate = 0.010},
            .model_used = std::nullopt,
        },
        provider::Response{
            .blocks = {core::TextContent{.text = "final"}},
            .stop_reason = core::StopReason::end_turn,
            .usage = provider::Usage{.input_tokens = 3,
                                     .output_tokens = 5,
                                     .cache_creation_tokens = 0,
                                     .cache_read_tokens = 1,
                                     .cost_estimate = 0.002},
            .model_used = std::string{"fake-trace-model"},
        },
    }};
    agent::Loop loop{provider, default_route()};
    tool::Registry registry;
    add_canned_tool(registry, canned_tool_def("file.read"), "read-ok");
    auto rules = allow_all_rules();
    permission::StorageAuditSink audit{audit_repo};
    auto ctx = dispatch_context(io, rules, audit);

    const auto catalog = registry.catalog();
    const std::vector<core::Message> tail{core::Message::user_text("trace read")};
    auto inputs = base_inputs(catalog, tail);
    inputs.turn_id = turn_id_with(0x31);
    inputs.trace = agent::TraceContext{
        .repository = &trace,
        .session_id = turn_id_with(0x80),
        .parent_turn_id = turn_id_with(0x22),
        .agent_key = "coder",
        .origin = "cli",
        .context_json = R"json({"source":"test"})json",
    };
    inputs.tools = &registry;
    inputs.dispatch_context = &ctx;
    auto result = co_await loop.run_turn(inputs);

    REQUIRE(result.has_value());
    REQUIRE(result->text == "final");
    REQUIRE(result->iterations == 2);

    auto row = co_await trace.get_turn(turn_id_with(0x31));
    REQUIRE(row.has_value());
    REQUIRE(row->has_value());
    REQUIRE((*row)->turn_id == turn_id_with(0x31));
    REQUIRE((*row)->parent_turn_id.has_value());
    REQUIRE(*(*row)->parent_turn_id == turn_id_with(0x22));
    REQUIRE((*row)->session_id == turn_id_with(0x80));
    REQUIRE((*row)->agent_key == "coder");
    REQUIRE((*row)->origin == "cli");
    REQUIRE((*row)->route_profile == "fake");
    REQUIRE((*row)->route_model == "fake-trace-model");
    REQUIRE((*row)->started_at_ns > 0);
    REQUIRE((*row)->finished_at_ns >= (*row)->started_at_ns);
    REQUIRE((*row)->stop_reason == "end_turn");
    REQUIRE((*row)->iteration_count == 2);
    REQUIRE((*row)->prompt_prefix_hash == result->rendered_prompt.prefix_hash);
    REQUIRE((*row)->prompt_prefix_bytes == static_cast<std::int64_t>(result->rendered_prompt.prefix_bytes));
    REQUIRE((*row)->active_catalog_hash == prompt_section_hash(result->rendered_prompt, "tool_catalog"));
    REQUIRE((*row)->deferred_catalog_hash == prompt_section_hash(result->rendered_prompt, "deferred_tools"));
    REQUIRE((*row)->cache_creation_tokens == 2);
    REQUIRE((*row)->cache_read_tokens == 8);
    REQUIRE((*row)->input_tokens == 14);
    REQUIRE((*row)->output_tokens == 6);
    REQUIRE((*row)->cost_estimate_usd == 0.012);
    REQUIRE_FALSE((*row)->cancellation_phase.has_value());
    REQUIRE((*row)->context_json == R"json({"source":"test"})json");

    auto events = co_await audit_repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A", .limit = 10});
    REQUIRE(events.has_value());
    REQUIRE(events->size() == 1);
    REQUIRE((*events)[0].tool_name == "file.read");
    REQUIRE((*events)[0].parent_turn_id.has_value());
    REQUIRE(*(*events)[0].parent_turn_id == (*row)->turn_id);
  });
}

TEST_CASE("Loop disables trace rows and audit parent ids when trace is off", "[unit][agent][loop][trace]") {
  TempDb db{"oran-agent-loop-trace-disabled"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_trace_pool(io, db);
    storage::TraceRepository trace{pool};
    storage::AuditRepository audit_repo{pool};
    auto migrated = co_await trace.migrate();
    REQUIRE(migrated.has_value());

    RecordingSequenceProvider provider{std::vector<provider::Response>{
        provider::Response{
            .blocks = {core::ToolUseContent{
                .id = "t1",
                .name = "file.read",
                .input_json = R"({"value":"a"})",
            }},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::nullopt,
        },
        provider::Response{
            .blocks = {core::TextContent{.text = "final"}},
            .stop_reason = core::StopReason::end_turn,
            .usage = {},
            .model_used = std::string{"fake-trace-model"},
        },
    }};
    agent::Loop loop{provider, default_route()};
    tool::Registry registry;
    add_canned_tool(registry, canned_tool_def("file.read"), "read-ok");
    auto rules = allow_all_rules();
    permission::StorageAuditSink audit{audit_repo};
    auto ctx = dispatch_context(io, rules, audit);
    ctx.parent_turn_id = turn_id_with(0x70);

    const auto catalog = registry.catalog();
    const std::vector<core::Message> tail{core::Message::user_text("trace disabled read")};
    auto inputs = base_inputs(catalog, tail);
    inputs.turn_id = turn_id_with(0x31);
    inputs.trace = agent::TraceContext{
        .enabled = false,
        .repository = &trace,
        .session_id = turn_id_with(0x80),
        .agent_key = "coder",
        .origin = "cli",
    };
    inputs.tools = &registry;
    inputs.dispatch_context = &ctx;
    auto result = co_await loop.run_turn(inputs);

    REQUIRE(result.has_value());
    REQUIRE(result->text == "final");

    auto count = co_await trace.count_turns();
    REQUIRE(count.has_value());
    REQUIRE(*count == 0);

    auto events = co_await audit_repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A", .limit = 10});
    REQUIRE(events.has_value());
    REQUIRE(events->size() == 1);
    REQUIRE((*events)[0].tool_name == "file.read");
    REQUIRE_FALSE((*events)[0].parent_turn_id.has_value());
    REQUIRE(ctx.parent_turn_id.has_value());
    REQUIRE(*ctx.parent_turn_id == turn_id_with(0x70));
  });
}

TEST_CASE("Loop persists provider cancellation trace rows", "[unit][agent][loop][trace][cancellation]") {
  TempDb db{"oran-agent-loop-trace-provider-cancel"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_trace_pool(io, db);
    storage::TraceRepository trace{pool};
    auto migrated = co_await trace.migrate();
    REQUIRE(migrated.has_value());

    asio::cancellation_signal signal;
    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response =
            provider::Response{
                .blocks = {core::TextContent{.text = "late"}},
                .stop_reason = core::StopReason::end_turn,
                .usage = {},
                .model_used = std::string{"never-used"},
            },
        .deltas = {},
        .error = std::nullopt,
        .latency = 1s,
    });
    provider::FakeProvider fake{std::move(plan)};
    agent::Loop loop{fake, default_route()};

    const auto catalog = loop_catalog();
    const std::vector<core::Message> tail{core::Message::user_text("cancel provider trace")};
    auto inputs = base_inputs(catalog, tail);
    inputs.turn_id = turn_id_with(0x41);
    inputs.trace = agent::TraceContext{
        .repository = &trace,
        .session_id = turn_id_with(0x80),
        .agent_key = "coder",
        .origin = "cli",
    };

    std::optional<core::Result<agent::RunTurnResult>> result;
    std::exception_ptr failure;
    asio::co_spawn(
        io,
        [&]() -> async::Awaitable<core::Result<agent::RunTurnResult>> { co_return co_await loop.run_turn(inputs); },
        asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, core::Result<agent::RunTurnResult> r) {
          failure = ep;
          result = std::move(r);
        }));

    asio::post(io, [&] { signal.emit(asio::cancellation_type::terminal); });
    while (!result.has_value() && !failure) {
      auto tick = co_await async::sleep_for(io.get_executor(), 1ms);
      REQUIRE(tick.has_value());
    }
    if (failure) {
      std::rethrow_exception(failure);
    }

    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->has_value());
    REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
    REQUIRE(contains_context(result->error(), "cancellation_phase", "provider"));

    auto row = co_await trace.get_turn(turn_id_with(0x41));
    REQUIRE(row.has_value());
    REQUIRE(row->has_value());
    REQUIRE((*row)->turn_id == turn_id_with(0x41));
    REQUIRE((*row)->session_id == turn_id_with(0x80));
    REQUIRE((*row)->route_model == "fake-1");
    REQUIRE((*row)->stop_reason == "cancelled");
    REQUIRE((*row)->iteration_count == 1);
    REQUIRE((*row)->input_tokens == 0);
    REQUIRE((*row)->output_tokens == 0);
    REQUIRE((*row)->cancellation_phase.has_value());
    REQUIRE(*(*row)->cancellation_phase == "provider");
  });
}

TEST_CASE("Loop persists tool cancellation trace rows", "[unit][agent][loop][trace][cancellation]") {
  TempDb db{"oran-agent-loop-trace-tool-cancel"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_trace_pool(io, db);
    storage::TraceRepository trace{pool};
    auto migrated = co_await trace.migrate();
    REQUIRE(migrated.has_value());

    asio::cancellation_signal signal;
    RecordingSequenceProvider provider{std::vector<provider::Response>{
        provider::Response{
            .blocks = {core::ToolUseContent{.id = "t1", .name = "file.read", .input_json = "{}"}},
            .stop_reason = core::StopReason::tool_use,
            .usage = provider::Usage{.input_tokens = 9,
                                     .output_tokens = 1,
                                     .cache_creation_tokens = 2,
                                     .cache_read_tokens = 3,
                                     .cost_estimate = 0.004},
            .model_used = std::string{"fake-tool-model"},
        },
    }};
    agent::Loop loop{provider, default_route()};
    tool::Registry registry;
    auto handler = [](std::string_view, tool::DispatchContext& ctx) -> async::Awaitable<core::Result<tool::Output>> {
      auto slept = co_await async::sleep_for(ctx.executor, 1s);
      if (!slept) {
        co_return std::unexpected(std::move(slept).error());
      }
      co_return tool::Output::text_only("late");
    };
    REQUIRE(registry.add(canned_tool_def("file.read"), std::move(handler)).has_value());
    auto rules = allow_all_rules();
    permission::RecordingAuditSink audit;
    auto ctx = dispatch_context(io, rules, audit);

    const auto catalog = registry.catalog();
    const std::vector<core::Message> tail{core::Message::user_text("cancel tool trace")};
    auto inputs = base_inputs(catalog, tail);
    inputs.turn_id = turn_id_with(0x42);
    inputs.trace = agent::TraceContext{
        .repository = &trace,
        .session_id = turn_id_with(0x80),
        .agent_key = "coder",
        .origin = "cli",
    };
    inputs.tools = &registry;
    inputs.dispatch_context = &ctx;

    std::optional<core::Result<agent::RunTurnResult>> result;
    std::exception_ptr failure;
    asio::co_spawn(
        io,
        [&]() -> async::Awaitable<core::Result<agent::RunTurnResult>> { co_return co_await loop.run_turn(inputs); },
        asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, core::Result<agent::RunTurnResult> r) {
          failure = ep;
          result = std::move(r);
        }));

    asio::post(io, [&] { signal.emit(asio::cancellation_type::terminal); });
    while (!result.has_value() && !failure) {
      auto tick = co_await async::sleep_for(io.get_executor(), 1ms);
      REQUIRE(tick.has_value());
    }
    if (failure) {
      std::rethrow_exception(failure);
    }

    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->has_value());
    REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
    REQUIRE(contains_context(result->error(), "cancellation_phase", "tools"));

    auto row = co_await trace.get_turn(turn_id_with(0x42));
    REQUIRE(row.has_value());
    REQUIRE(row->has_value());
    REQUIRE((*row)->turn_id == turn_id_with(0x42));
    REQUIRE((*row)->session_id == turn_id_with(0x80));
    REQUIRE((*row)->route_model == "fake-tool-model");
    REQUIRE((*row)->stop_reason == "cancelled");
    REQUIRE((*row)->iteration_count == 1);
    REQUIRE((*row)->input_tokens == 9);
    REQUIRE((*row)->output_tokens == 1);
    REQUIRE((*row)->cache_creation_tokens == 2);
    REQUIRE((*row)->cache_read_tokens == 3);
    REQUIRE((*row)->cost_estimate_usd == 0.004);
    REQUIRE((*row)->cancellation_phase.has_value());
    REQUIRE(*(*row)->cancellation_phase == "tools");
    REQUIRE(audit.events().size() == 1);
  });
}

TEST_CASE("Loop preserves multiple tool_results in tool_use order", "[unit][agent][loop]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    RecordingSequenceProvider provider{std::vector<provider::Response>{
        provider::Response{
            .blocks =
                {
                    core::ToolUseContent{.id = "b", .name = "second.tool", .input_json = R"({"value":"2"})"},
                    core::ToolUseContent{.id = "a", .name = "first.tool", .input_json = R"({"value":"1"})"},
                },
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::nullopt,
        },
        provider::Response{
            .blocks = {core::TextContent{.text = "ordered"}},
            .stop_reason = core::StopReason::end_turn,
            .usage = {},
            .model_used = std::nullopt,
        },
    }};
    agent::Loop loop{provider, default_route()};
    tool::Registry registry;
    add_canned_tool(registry, canned_tool_def("first.tool"), "first");
    add_canned_tool(registry, canned_tool_def("second.tool"), "second");
    auto rules = allow_all_rules();
    permission::RecordingAuditSink audit;
    auto ctx = dispatch_context(io, rules, audit);
    ctx.parent_turn_id = turn_id_with(0x70);

    const auto catalog = registry.catalog();
    const std::vector<core::Message> tail{core::Message::user_text("run both")};
    auto inputs = base_inputs(catalog, tail);
    inputs.active_tools = config::PromptActiveToolsConfig{
        .use_defaults = false,
        .tool_names = {"first.tool", "second.tool"},
    };
    inputs.tools = &registry;
    inputs.dispatch_context = &ctx;
    auto result = co_await loop.run_turn(inputs);

    REQUIRE(result.has_value());
    REQUIRE(provider.requests().size() == 2);
    REQUIRE(provider.requests()[1].messages.size() == 3);
    const auto& tool_message = provider.requests()[1].messages[2];
    REQUIRE(tool_message.role == core::Role::tool);
    REQUIRE(tool_message.blocks.size() == 2);
    REQUIRE(tool_result_at(tool_message, 0).tool_use_id == "b");
    REQUIRE(tool_result_at(tool_message, 0).output == R"(second:{"value":"2"})");
    REQUIRE(tool_result_at(tool_message, 1).tool_use_id == "a");
    REQUIRE(tool_result_at(tool_message, 1).output == R"(first:{"value":"1"})");
    REQUIRE(audit.events().size() == 2);
    REQUIRE(audit.events()[0].tool_name == "second.tool");
    REQUIRE(audit.events()[1].tool_name == "first.tool");
    REQUIRE_FALSE(audit.events()[0].parent_turn_id.has_value());
    REQUIRE_FALSE(audit.events()[1].parent_turn_id.has_value());
    REQUIRE(ctx.parent_turn_id.has_value());
    REQUIRE(*ctx.parent_turn_id == turn_id_with(0x70));
  });
}

TEST_CASE("Loop returns model-visible tool errors as tool_result blocks", "[unit][agent][loop]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    RecordingSequenceProvider provider{std::vector<provider::Response>{
        provider::Response{
            .blocks = {core::ToolUseContent{.id = "missing", .name = "tool.missing", .input_json = "{}"}},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::nullopt,
        },
        provider::Response{
            .blocks = {core::TextContent{.text = "repaired"}},
            .stop_reason = core::StopReason::end_turn,
            .usage = {},
            .model_used = std::nullopt,
        },
    }};
    agent::Loop loop{provider, default_route()};
    tool::Registry registry;
    auto rules = allow_all_rules();
    permission::RecordingAuditSink audit;
    auto ctx = dispatch_context(io, rules, audit);

    const auto catalog = registry.catalog();
    const std::vector<core::Message> tail{core::Message::user_text("missing")};
    auto inputs = base_inputs(catalog, tail);
    inputs.tools = &registry;
    inputs.dispatch_context = &ctx;
    auto result = co_await loop.run_turn(inputs);

    REQUIRE(result.has_value());
    REQUIRE(provider.requests().size() == 2);
    const auto& tool_result = tool_result_at(provider.requests()[1].messages[2], 0);
    REQUIRE(tool_result.tool_use_id == "missing");
    REQUIRE(tool_result.is_error);
    REQUIRE(tool_result.output.contains("tool error: tool is not registered"));
    REQUIRE(tool_result.output.contains("tool: tool.missing"));
  });
}

TEST_CASE("Loop propagates infrastructure errors from tool dispatch", "[unit][agent][loop]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    RecordingSequenceProvider provider{std::vector<provider::Response>{
        provider::Response{
            .blocks = {core::ToolUseContent{.id = "t1", .name = "file.read", .input_json = "{}"}},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::nullopt,
        },
    }};
    agent::Loop loop{provider, default_route()};
    tool::Registry registry;
    add_canned_tool(registry, canned_tool_def("file.read"), "audit broke", false, core::ErrorKind::internal);
    auto rules = allow_all_rules();
    permission::RecordingAuditSink audit;
    auto ctx = dispatch_context(io, rules, audit);

    const auto catalog = registry.catalog();
    const std::vector<core::Message> tail{core::Message::user_text("read")};
    auto inputs = base_inputs(catalog, tail);
    inputs.tools = &registry;
    inputs.dispatch_context = &ctx;
    auto result = co_await loop.run_turn(inputs);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::internal);
    REQUIRE(result.error().message() == "audit broke");
  });
}

TEST_CASE("Loop annotates cancellation during tool dispatch", "[unit][agent][loop][cancellation]") {
  asio::io_context io;
  asio::cancellation_signal signal;

  RecordingSequenceProvider provider{std::vector<provider::Response>{
      provider::Response{
          .blocks = {core::ToolUseContent{.id = "t1", .name = "file.read", .input_json = "{}"}},
          .stop_reason = core::StopReason::tool_use,
          .usage = {},
          .model_used = std::nullopt,
      },
  }};
  agent::Loop loop{provider, default_route()};
  tool::Registry registry;
  auto handler = [](std::string_view, tool::DispatchContext& ctx) -> async::Awaitable<core::Result<tool::Output>> {
    auto slept = co_await async::sleep_for(ctx.executor, 1s);
    if (!slept) {
      co_return std::unexpected(std::move(slept).error());
    }
    co_return tool::Output::text_only("late");
  };
  REQUIRE(registry.add(canned_tool_def("file.read"), std::move(handler)).has_value());
  auto rules = allow_all_rules();
  permission::RecordingAuditSink audit;
  auto ctx = dispatch_context(io, rules, audit);

  const auto catalog = registry.catalog();
  const std::vector<core::Message> tail{core::Message::user_text("cancel tool")};
  auto inputs = base_inputs(catalog, tail);
  inputs.tools = &registry;
  inputs.dispatch_context = &ctx;
  std::optional<core::Result<agent::RunTurnResult>> result;
  std::exception_ptr failure;

  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<agent::RunTurnResult>> { co_return co_await loop.run_turn(inputs); },
      asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, core::Result<agent::RunTurnResult> r) {
        failure = ep;
        result = std::move(r);
        io.stop();
      }));

  asio::post(io, [&] { signal.emit(asio::cancellation_type::terminal); });
  io.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
  REQUIRE(contains_context(result->error(), "reason", "parent_cancelled"));
  REQUIRE(contains_context(result->error(), "cancellation_phase", "tools"));
  REQUIRE(provider.requests().size() == 1);
  REQUIRE(audit.events().size() == 1);
}

TEST_CASE("Loop stops repeated tool_use turns at the iteration cap", "[unit][agent][loop]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    RecordingSequenceProvider provider{std::vector<provider::Response>{
        provider::Response{
            .blocks = {core::ToolUseContent{.id = "t1", .name = "file.read", .input_json = "{}"}},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::nullopt,
        },
        provider::Response{
            .blocks = {core::ToolUseContent{.id = "t2", .name = "file.read", .input_json = "{}"}},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::nullopt,
        },
    }};
    agent::Loop loop{provider, default_route(), agent::LoopOptions{.max_iterations = 2}};
    tool::Registry registry;
    add_canned_tool(registry, canned_tool_def("file.read"), "read");
    auto rules = allow_all_rules();
    permission::RecordingAuditSink audit;
    auto ctx = dispatch_context(io, rules, audit);

    const auto catalog = registry.catalog();
    const std::vector<core::Message> tail{core::Message::user_text("loop")};
    auto inputs = base_inputs(catalog, tail);
    inputs.tools = &registry;
    inputs.dispatch_context = &ctx;
    auto result = co_await loop.run_turn(inputs);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::internal);
    REQUIRE(contains_context(result.error(), "reason", "iteration_cap"));
    REQUIRE(contains_context(result.error(), "max_iterations", "2"));
    REQUIRE(provider.requests().size() == 2);
    REQUIRE(audit.events().size() == 2);
  });
}
