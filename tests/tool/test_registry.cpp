// tests/tool/test_registry.cpp — registry add/find/catalog/dispatch coverage.

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/cancellation_type.hpp>
#include <asio/co_spawn.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/this_coro.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <oran/async.hpp>
#include <oran/core/capability.hpp>
#include <oran/core/error.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/core/turn_id.hpp>
#include <oran/hook.hpp>
#include <oran/permission.hpp>
#include <oran/storage.hpp>
#include <oran/tool.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace permission = orangutan::permission;
namespace storage = orangutan::storage;
namespace tool = orangutan::tool;
namespace test = orangutan::tests;

namespace {

class TempFile {
public:
  explicit TempFile(std::string suffix)
      : path_(std::filesystem::temp_directory_path() /
              ("oran-tool-" + std::move(suffix) + "-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {}

  ~TempFile() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }

  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  void write(std::string_view contents) const {
    std::ofstream out{path_, std::ios::binary};
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  }

  [[nodiscard]] std::string string() const {
    return path_.string();
  }

private:
  std::filesystem::path path_;
};

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

async::Awaitable<core::Result<tool::Output>> echo_handler(std::string_view input, tool::DispatchContext& /*ctx*/) {
  co_return tool::Output{.text = std::string{input}};
}

tool::Handler make_echo_handler() {
  return &echo_handler;
}

permission::RuleSet single_rule(permission::Rule rule) {
  permission::RuleSet rs;
  rs.push_back(std::move(rule));
  return rs;
}

tool::DispatchContext make_ctx(asio::io_context& io,
                               permission::RuleSet& rules,
                               permission::AuditSink& sink,
                               permission::Mode mode = permission::Mode::default_) {
  return tool::DispatchContext{
      .executor = io.get_executor(),
      .mode = mode,
      .rules = rules,
      .audit = sink,
      .scope_key = "scope-A",
      .agent_key = "coder",
      .identity = "operator-1",
  };
}

tool::DispatchContext make_temp_workspace_ctx(asio::io_context& io,
                                              permission::RuleSet& rules,
                                              permission::AuditSink& sink,
                                              permission::Mode mode = permission::Mode::default_) {
  static auto workspace = [] {
    auto created = tool::Workspace::create(std::filesystem::temp_directory_path().string());
    if (!created) {
      throw std::runtime_error{"failed to create temporary-directory workspace"};
    }
    return std::move(*created);
  }();

  auto ctx = make_ctx(io, rules, sink, mode);
  ctx.workspace = &workspace;
  return ctx;
}

core::TurnId turn_id_with(unsigned char seed) {
  core::TurnId id{};
  for (std::size_t i = 0; i < id.size(); ++i) {
    id[i] = static_cast<std::byte>(seed + i);
  }
  return id;
}

[[nodiscard]] bool context_has(const core::Error& error, std::string_view key, std::string_view value) {
  return std::ranges::any_of(error.context(),
                             [&](const auto& entry) { return entry.first == key && entry.second == value; });
}

}  // namespace

TEST_CASE("Registry::add rejects empty name", "[unit][tool][registry]") {
  tool::Registry registry;
  auto added = registry.add(core::ToolDef{}, make_echo_handler());
  REQUIRE_FALSE(added.has_value());
  REQUIRE(added.error().kind() == core::ErrorKind::invalid_argument);
}

TEST_CASE("Registry::add rejects empty handler", "[unit][tool][registry]") {
  tool::Registry registry;
  auto added = registry.add(core::ToolDef::with_no_input("noop", "noop"), tool::Handler{});
  REQUIRE_FALSE(added.has_value());
  REQUIRE(added.error().kind() == core::ErrorKind::invalid_argument);
}

TEST_CASE("Registry::add rejects invalid input_schema_json", "[unit][tool][registry]") {
  tool::Registry registry;
  auto def = core::ToolDef::with_no_input("BadSchema", "bad schema");
  def.input_schema_json = R"({"type":"object","properties":)";

  auto added = registry.add(std::move(def), make_echo_handler());
  REQUIRE_FALSE(added.has_value());
  REQUIRE(added.error().kind() == core::ErrorKind::invalid_argument);
  REQUIRE(context_has(added.error(), "tool", "BadSchema"));
  REQUIRE(context_has(added.error(), "schema_path", "$"));
  REQUIRE(registry.size() == 0);
}

TEST_CASE("Registry::add rejects malformed JSON Schema keywords", "[unit][tool][registry]") {
  tool::Registry registry;
  auto def = core::ToolDef::with_no_input("BadRequired", "bad required");
  def.input_schema_json =
      R"({"type":"object","properties":{"path":{"type":"string"}},"required":"path","additionalProperties":false})";

  auto added = registry.add(std::move(def), make_echo_handler());
  REQUIRE_FALSE(added.has_value());
  REQUIRE(added.error().kind() == core::ErrorKind::invalid_argument);
  REQUIRE(context_has(added.error(), "tool", "BadRequired"));
  REQUIRE(context_has(added.error(), "schema_path", "$.required"));
  REQUIRE(registry.size() == 0);
}

TEST_CASE("DispatchContext::for_now creates a fresh wall-clock context", "[unit][tool][registry][context]") {
  asio::io_context io;
  auto rules = single_rule(permission::Rule{
      .verdict = permission::Verdict::allow,
      .tool_pattern = "*",
      .capability = std::nullopt,
  });
  permission::NullAuditSink audit;

  auto ctx = tool::DispatchContext::for_now(io.get_executor(), rules, audit, "scope-A", "coder", "operator-1");

  REQUIRE(ctx.executor == io.get_executor());
  REQUIRE(ctx.mode == permission::Mode::default_);
  REQUIRE(ctx.now > core::Time::epoch());
  REQUIRE(ctx.scope_key == "scope-A");
  REQUIRE(ctx.agent_key == "coder");
  REQUIRE(ctx.identity == "operator-1");
  REQUIRE(ctx.registry == nullptr);
  REQUIRE_FALSE(ctx.resolved_path.has_value());
}

TEST_CASE("DispatchContext::for_now clones a prototype and clears dispatch-local fields",
          "[unit][tool][registry][context]") {
  asio::io_context io;
  auto rules = single_rule(permission::Rule{
      .verdict = permission::Verdict::allow,
      .tool_pattern = "*",
      .capability = std::nullopt,
  });
  permission::NullAuditSink audit;
  tool::Registry registry;
  permission::ApprovalToken token_output;

  auto prototype = make_ctx(io, rules, audit, permission::Mode::strict);
  prototype.registry = &registry;
  prototype.resolved_path = tool::ResolvedToolPath{
      .authority_relative_path = "a",
      .absolute_path = "/tmp/a",
      .relative_path = "a",
      .display_path = "<workspace>/a",
      .input_path_hash = "input-hash",
      .workspace_root_hash = "root-hash",
  };
  prototype.approval_token_output = &token_output;
  prototype.parent_turn_id = turn_id_with(7);
  prototype.now = core::Time::epoch();

  auto threaded = tool::DispatchContext::for_now(prototype, /*thread_approval_token_output=*/true);
  auto dropped = tool::DispatchContext::for_now(prototype, /*thread_approval_token_output=*/false);

  REQUIRE(threaded.mode == permission::Mode::strict);
  REQUIRE(threaded.approval_token_output == &token_output);
  REQUIRE(threaded.parent_turn_id == prototype.parent_turn_id);
  REQUIRE(threaded.now > core::Time::epoch());
  REQUIRE(threaded.registry == nullptr);
  REQUIRE_FALSE(threaded.resolved_path.has_value());

  REQUIRE(dropped.approval_token_output == nullptr);
  REQUIRE(dropped.scope_key == prototype.scope_key);
  REQUIRE(dropped.agent_key == prototype.agent_key);
  REQUIRE(dropped.identity == prototype.identity);
}

TEST_CASE("Registry::add rejects duplicates", "[unit][tool][registry]") {
  tool::Registry registry;
  REQUIRE(registry.add(core::ToolDef::with_no_input("noop", "noop"), make_echo_handler()).has_value());
  auto second = registry.add(core::ToolDef::with_no_input("noop", "noop"), make_echo_handler());
  REQUIRE_FALSE(second.has_value());
  REQUIRE(second.error().kind() == core::ErrorKind::conflict);
  REQUIRE(registry.size() == 1);
}

TEST_CASE("Registry::find returns nullptr for unknown names and a pointer for known ones", "[unit][tool][registry]") {
  tool::Registry registry;
  REQUIRE(registry.add(core::ToolDef::with_no_input("alpha", "alpha"), make_echo_handler()).has_value());
  REQUIRE(registry.find("alpha") != nullptr);
  REQUIRE(registry.find("alpha")->name == "alpha");
  REQUIRE(registry.find("missing") == nullptr);
}

TEST_CASE("Registry::catalog reports tools in insertion order", "[unit][tool][registry]") {
  tool::Registry registry;
  REQUIRE(registry.add(core::ToolDef::with_no_input("alpha", "alpha"), make_echo_handler()).has_value());
  REQUIRE(registry.add(core::ToolDef::with_no_input("beta", "beta"), make_echo_handler()).has_value());
  REQUIRE(registry.add(core::ToolDef::with_no_input("gamma", "gamma"), make_echo_handler()).has_value());
  const auto catalog = registry.catalog();
  REQUIRE(catalog.size() == 3);
  REQUIRE(catalog[0].name == "alpha");
  REQUIRE(catalog[1].name == "beta");
  REQUIRE(catalog[2].name == "gamma");
}

TEST_CASE("CatalogRenderer renders a deterministic full-schema tool block", "[unit][tool][catalog]") {
  core::ToolDef def{
      .name = "FileRead",
      .description = "Read a UTF-8 text file.",
      .input_schema_json =
          R"({"required":["path"],"properties":{"path":{"type":"string"}},"type":"object","additionalProperties":false})",
      .required_capabilities = {core::Capability::read_file},
      .deferred = false,
      .category = "file",
  };

  tool::CatalogRenderer renderer;
  auto first = renderer.render_tool_block(def);
  REQUIRE(first.has_value());
  auto second = renderer.render_tool_block(def);
  REQUIRE(second.has_value());

  REQUIRE(*first == *second);
  REQUIRE(first->contains("Tool: FileRead\n"));
  REQUIRE(first->contains("Description: Read a UTF-8 text file.\n"));
  REQUIRE(first->contains("Category: file\n"));
  REQUIRE(first->contains("Capabilities: read_file\n"));
  REQUIRE(first->contains("Input Schema:\n"));
  REQUIRE(first->contains(R"("additionalProperties": false)"));
  REQUIRE(first->contains(R"("path")"));

  const auto stats = renderer.cache_stats();
  REQUIRE(stats.renderer_version == 1);
  REQUIRE(stats.blocks.misses == 1);
  REQUIRE(stats.blocks.hits == 1);
  REQUIRE(stats.blocks.current_entries == 1);
}

TEST_CASE("CatalogRenderer sorts active tools and separates deferred entries", "[unit][tool][catalog]") {
  auto active_b = core::ToolDef::with_no_input("FileWrite", "Write a file.");
  active_b.required_capabilities = {core::Capability::write_file};
  active_b.category = "file";

  auto deferred = core::ToolDef::with_no_input("MemoryRecall", "Recall memory.");
  deferred.required_capabilities = {core::Capability::read_memory};
  deferred.deferred = true;
  deferred.category = "memory";

  auto active_a = core::ToolDef::with_no_input("FileRead", "Read a file.");
  active_a.required_capabilities = {core::Capability::read_file};
  active_a.category = "file";

  const std::vector<core::ToolDef> defs{active_b, deferred, active_a};
  tool::CatalogRenderer renderer;
  auto rendered = renderer.render_catalog(defs);
  REQUIRE(rendered.has_value());

  REQUIRE(rendered->active_blocks.size() == 2);
  REQUIRE(rendered->active_blocks[0].starts_with("Tool: FileRead\n"));
  REQUIRE(rendered->active_blocks[1].starts_with("Tool: FileWrite\n"));
  REQUIRE(rendered->active_text.find("Tool: FileRead") < rendered->active_text.find("Tool: FileWrite"));
  REQUIRE_FALSE(rendered->active_text.contains("MemoryRecall"));
  REQUIRE(rendered->deferred_entries == std::vector<std::string>{"MemoryRecall - Recall memory."});
  REQUIRE(rendered->deferred_text == "MemoryRecall - Recall memory.");
}

TEST_CASE("CatalogRenderer cache key includes renderer version and rendered ToolDef fields", "[unit][tool][catalog]") {
  auto def = core::ToolDef::with_no_input("AlphaTool", "Alpha.");
  def.required_capabilities = {core::Capability::read_file};
  def.category = "alpha";

  tool::CatalogRenderer v1{tool::ToolCatalogRenderOptions{.renderer_version = 1, .max_cached_blocks = 256}};
  tool::CatalogRenderer v2{tool::ToolCatalogRenderOptions{.renderer_version = 2, .max_cached_blocks = 256}};

  const auto hash_v1 = tool::tool_def_render_hash(def, 1);
  const auto hash_v2 = tool::tool_def_render_hash(def, 2);
  REQUIRE(hash_v1 != hash_v2);

  auto deferred_only = def;
  deferred_only.deferred = true;
  REQUIRE(tool::tool_def_render_hash(def, 1) == tool::tool_def_render_hash(deferred_only, 1));

  auto category_changed = def;
  category_changed.category = "beta";
  REQUIRE(tool::tool_def_render_hash(def, 1) != tool::tool_def_render_hash(category_changed, 1));

  REQUIRE(v1.render_tool_block(def).has_value());
  REQUIRE(v1.render_tool_block(def).has_value());
  REQUIRE(v1.cache_stats().blocks.misses == 1);
  REQUIRE(v1.cache_stats().blocks.hits == 1);

  REQUIRE(v2.render_tool_block(def).has_value());
  REQUIRE(v2.cache_stats().renderer_version == 2);
  REQUIRE(v2.cache_stats().blocks.misses == 1);
  REQUIRE(v2.cache_stats().blocks.hits == 0);
}

TEST_CASE("CatalogRenderer can disable memoisation without unbounded state", "[unit][tool][catalog]") {
  auto def = core::ToolDef::with_no_input("AlphaTool", "Alpha.");
  tool::CatalogRenderer renderer{tool::ToolCatalogRenderOptions{.renderer_version = 1, .max_cached_blocks = 0}};

  REQUIRE(renderer.render_tool_block(def).has_value());
  REQUIRE(renderer.render_tool_block(def).has_value());

  const auto stats = renderer.cache_stats();
  REQUIRE(stats.blocks.hits == 0);
  REQUIRE(stats.blocks.misses == 2);
  REQUIRE(stats.blocks.current_entries == 0);
  REQUIRE(stats.blocks.current_bytes == 0);
}

TEST_CASE("Registry::remove unregisters tools and reports not_found on a second call", "[unit][tool][registry]") {
  tool::Registry registry;
  REQUIRE(registry.add(core::ToolDef::with_no_input("alpha", "alpha"), make_echo_handler()).has_value());
  REQUIRE(registry.remove("alpha").has_value());
  REQUIRE(registry.find("alpha") == nullptr);
  auto second = registry.remove("alpha");
  REQUIRE_FALSE(second.has_value());
  REQUIRE(second.error().kind() == core::ErrorKind::not_found);
}

TEST_CASE("Registry::dispatch reports not_found for an unknown tool and records nothing", "[unit][tool][registry]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    permission::RuleSet rules;
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink);

    auto result = co_await registry.dispatch("missing", "{}", ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::not_found);
    REQUIRE(sink.events().empty());
  });
}

TEST_CASE("Registry::dispatch records one allow event and returns the handler output", "[unit][tool][registry]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(core::ToolDef::with_no_input("noop", "noop"), make_echo_handler()).has_value());

    auto rules = single_rule(permission::Rule{.verdict = permission::Verdict::allow, .tool_pattern = "noop"});
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink);
    ctx.parent_turn_id = turn_id_with(0x50);

    const std::string_view input = R"({"hello":"world"})";
    auto result = co_await registry.dispatch("noop", input, ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text == input);

    REQUIRE(sink.events().size() == 1);
    const auto& event = sink.events()[0];
    REQUIRE(event.tool_name == "noop");
    REQUIRE(event.scope_key == "scope-A");
    REQUIRE(event.agent_key == "coder");
    REQUIRE(event.identity == "operator-1");
    REQUIRE(event.verdict == permission::Verdict::allow);
    REQUIRE(event.outcome == permission::AuditOutcome::allow);
    REQUIRE(event.input_hash.has_value());
    REQUIRE(event.parent_turn_id.has_value());
    REQUIRE(*event.parent_turn_id == turn_id_with(0x50));

    const auto expected_hash = permission::ApprovalAuthority::input_hash(input);
    REQUIRE(*event.input_hash == expected_hash);
  });
}

TEST_CASE("Registry::dispatch records a deny event and returns permission_denied", "[unit][tool][registry]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(core::ToolDef::with_no_input("noop", "noop"), make_echo_handler()).has_value());

    auto rules = single_rule(permission::Rule{.verdict = permission::Verdict::deny, .tool_pattern = "noop"});
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink);

    auto result = co_await registry.dispatch("noop", "{}", ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(context_has(result.error(), "tool", "noop"));

    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].verdict == permission::Verdict::deny);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::deny);
  });
}

TEST_CASE("Registry::dispatch reports ask as approval_required and records outcome=ask", "[unit][tool][registry]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(core::ToolDef::with_no_input("noop", "noop"), make_echo_handler()).has_value());

    auto rules = single_rule(permission::Rule{.verdict = permission::Verdict::ask, .tool_pattern = "noop"});
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink);

    auto result = co_await registry.dispatch("noop", "{}", ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(context_has(result.error(), "reason", "approval_required"));
    // Slice 21: the error now carries replay_max + approval_ttl_seconds +
    // decision_reason copied from the matched rule so the agent loop can
    // hand them straight to `ApprovalBroker::approve` without re-running
    // rule evaluation. Rule defaults are 8 / 3600s; we assert both
    // verbatim to pin the wire spelling.
    REQUIRE(context_has(result.error(), "replay_max", "8"));
    REQUIRE(context_has(result.error(), "approval_ttl_seconds", "3600"));
    REQUIRE(context_has(result.error(), "decision_reason", "rule #0 (ask: noop)"));

    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].verdict == permission::Verdict::ask);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::ask);
  });
}

TEST_CASE("Registry::dispatch propagates custom replay_max / approval_ttl_seconds on the approval_required error",
          "[unit][tool][registry][approval]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(core::ToolDef::with_no_input("noop", "noop"), make_echo_handler()).has_value());

    auto rules = single_rule(permission::Rule{
        .verdict = permission::Verdict::ask,
        .tool_pattern = "noop",
        .replay_max = 2U,
        .approval_ttl = std::chrono::seconds{120},
    });
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink);

    auto result = co_await registry.dispatch("noop", "{}", ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(context_has(result.error(), "reason", "approval_required"));
    REQUIRE(context_has(result.error(), "replay_max", "2"));
    REQUIRE(context_has(result.error(), "approval_ttl_seconds", "120"));
  });
}

TEST_CASE("Registry::dispatch honors a capability scope on the firing rule", "[unit][tool][registry][capability]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    core::ToolDef def{
        .name = "NeedsRead",
        .description = "tool needing read_file",
        .input_schema_json = "{}",
        .required_capabilities = {core::Capability::read_file},
        .deferred = false,
        .category = {},
    };
    REQUIRE(registry.add(std::move(def), make_echo_handler()).has_value());

    // Capability that does NOT match the tool's declared `read_file` — under
    // `Mode::strict` the call falls through to the mode default (deny).
    auto wrong_cap = single_rule(permission::Rule{
        .verdict = permission::Verdict::allow,
        .tool_pattern = "Needs*",
        .capability = core::Capability::write_file,
    });
    permission::RecordingAuditSink sink_wrong;
    auto ctx_wrong = make_ctx(io, wrong_cap, sink_wrong, permission::Mode::strict);
    auto wrong = co_await registry.dispatch("NeedsRead", "{}", ctx_wrong);
    REQUIRE_FALSE(wrong.has_value());
    REQUIRE(wrong.error().kind() == core::ErrorKind::permission_denied);

    // Capability that DOES match — the rule fires and allow flows through.
    auto right_cap = single_rule(permission::Rule{
        .verdict = permission::Verdict::allow,
        .tool_pattern = "Needs*",
        .capability = core::Capability::read_file,
    });
    permission::RecordingAuditSink sink_right;
    auto ctx_right = make_ctx(io, right_cap, sink_right, permission::Mode::strict);
    auto right = co_await registry.dispatch("NeedsRead", "{}", ctx_right);
    REQUIRE(right.has_value());
  });
}

TEST_CASE("Registry::dispatch surfaces audit-sink errors instead of swallowing them", "[unit][tool][registry][audit]") {
  class FailingSink : public permission::AuditSink {
  public:
    async::Awaitable<core::Result<void>> record(permission::AuditEvent /*event*/) override {
      co_return std::unexpected(core::Error::storage("simulated audit failure"));
    }
  };

  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(core::ToolDef::with_no_input("noop", "noop"), make_echo_handler()).has_value());
    auto rules = single_rule(permission::Rule{.verdict = permission::Verdict::allow, .tool_pattern = "noop"});
    FailingSink sink;
    auto ctx = make_ctx(io, rules, sink);

    auto result = co_await registry.dispatch("noop", "{}", ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::storage);
  });
}

TEST_CASE("register_file_read advertises a `read_file` capability and a path schema", "[unit][tool][file_read]") {
  tool::Registry registry;
  REQUIRE(tool::register_file_read(registry).has_value());
  REQUIRE(registry.size() == 1);
  const auto* def = registry.find(tool::kFileReadName);
  REQUIRE(def != nullptr);
  REQUIRE(def->required_capabilities.size() == 1);
  REQUIRE(def->required_capabilities[0] == core::Capability::read_file);
  REQUIRE(def->input_schema_json.contains("\"path\""));
}

TEST_CASE("register_tool_search advertises a capability-free runtime lookup", "[unit][tool][tool_search]") {
  tool::Registry registry;
  REQUIRE(tool::register_tool_search(registry).has_value());
  REQUIRE(registry.size() == 1);
  const auto* def = registry.find(tool::kToolSearchName);
  REQUIRE(def != nullptr);
  REQUIRE(def->required_capabilities.empty());
  REQUIRE_FALSE(def->deferred);
  REQUIRE(def->category.has_value());
  REQUIRE(*def->category == "runtime");
  REQUIRE(def->input_schema_json.contains("\"name\""));
  REQUIRE(def->input_schema_json.contains("\"category\""));
  REQUIRE(def->input_schema_json.contains("\"capability\""));
}

TEST_CASE("register_memory_recall advertises read_memory and deferred memory metadata", "[unit][tool][memory_recall]") {
  tool::Registry registry;
  REQUIRE(tool::register_memory_recall(registry).has_value());
  REQUIRE(registry.size() == 1);
  const auto* def = registry.find(tool::kMemoryRecallName);
  REQUIRE(def != nullptr);
  REQUIRE(def->required_capabilities.size() == 1);
  REQUIRE(def->required_capabilities[0] == core::Capability::read_memory);
  REQUIRE(def->deferred);
  REQUIRE(def->category.has_value());
  REQUIRE(*def->category == "memory");
  REQUIRE(def->input_schema_json.contains("\"query\""));
  REQUIRE(def->input_schema_json.contains("\"limit\""));
  REQUIRE(def->input_schema_json.contains("\"kinds\""));
}

TEST_CASE("register_memory_remember advertises write_memory and deferred memory metadata",
          "[unit][tool][memory_remember]") {
  tool::Registry registry;
  REQUIRE(tool::register_memory_remember(registry).has_value());
  REQUIRE(registry.size() == 1);
  const auto* def = registry.find(tool::kMemoryRememberName);
  REQUIRE(def != nullptr);
  REQUIRE(def->required_capabilities.size() == 1);
  REQUIRE(def->required_capabilities[0] == core::Capability::write_memory);
  REQUIRE(def->deferred);
  REQUIRE(def->category.has_value());
  REQUIRE(*def->category == "memory");
  REQUIRE(def->input_schema_json.contains("\"id\""));
  REQUIRE(def->input_schema_json.contains("\"kind\""));
  REQUIRE(def->input_schema_json.contains("\"title\""));
  REQUIRE(def->input_schema_json.contains("\"body\""));
  REQUIRE(def->input_schema_json.contains("\"importance\""));
}

TEST_CASE("register_memory_forget advertises write_memory and deferred memory metadata",
          "[unit][tool][memory_forget]") {
  tool::Registry registry;
  REQUIRE(tool::register_memory_forget(registry).has_value());
  REQUIRE(registry.size() == 1);
  const auto* def = registry.find(tool::kMemoryForgetName);
  REQUIRE(def != nullptr);
  REQUIRE(def->required_capabilities.size() == 1);
  REQUIRE(def->required_capabilities[0] == core::Capability::write_memory);
  REQUIRE(def->deferred);
  REQUIRE(def->category.has_value());
  REQUIRE(*def->category == "memory");
  REQUIRE(def->input_schema_json.contains("\"id\""));
}

TEST_CASE("register_builtins seeds the file tool catalog", "[unit][tool][builtins]") {
  tool::Registry registry;
  REQUIRE(tool::register_builtins(registry).has_value());
  const auto catalog = registry.catalog();
  REQUIRE(catalog.size() == 4);
  REQUIRE(catalog[0].name == tool::kFileReadName);
  REQUIRE(catalog[1].name == tool::kFileWriteName);
  REQUIRE(catalog[2].name == tool::kFileEditName);
  REQUIRE(catalog[3].name == tool::kToolSearchName);
}

TEST_CASE("ToolSearch returns structured tool metadata by exact name", "[unit][tool][tool_search]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_read(registry).has_value());
    REQUIRE(tool::register_tool_search(registry).has_value());

    auto rules = single_rule(permission::Rule{
        .verdict = permission::Verdict::allow,
        .tool_pattern = std::string{tool::kToolSearchName},
    });
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    auto result = co_await registry.dispatch(tool::kToolSearchName, R"({"name":"FileRead"})", ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text.starts_with("ToolSearch: 1 match"));
    REQUIRE(result->text.contains("FileRead"));
    REQUIRE(result->data_json.has_value());
    const auto data = nlohmann::json::parse(*result->data_json);
    REQUIRE(data["kind"] == "tool_search");
    REQUIRE(data["query"]["name"] == "FileRead");
    REQUIRE(data["match_count"] == 1);
    REQUIRE(data["matches"].size() == 1);
    const auto& match = data["matches"][0];
    REQUIRE(match["name"] == "FileRead");
    REQUIRE(match["category"] == "file");
    REQUIRE(match["deferred"] == false);
    REQUIRE(match["description"].get<std::string>().contains("Read"));
    REQUIRE(match["input_schema"]["properties"].contains("path"));
    REQUIRE(match["required_capabilities"] == nlohmann::json::array({"read_file"}));
    REQUIRE(result->usage.match_count.has_value());
    REQUIRE(*result->usage.match_count == 1);
  });
}

TEST_CASE("ToolSearch filters late-registered deferred tools by category and capability", "[unit][tool][tool_search]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_tool_search(registry).has_value());

    auto memory = core::ToolDef::with_no_input("MemoryRecall", "Recall long-term memory.");
    memory.required_capabilities = {core::Capability::read_memory};
    memory.deferred = true;
    memory.category = "memory";
    REQUIRE(registry.add(std::move(memory), make_echo_handler()).has_value());

    auto rules = single_rule(permission::Rule{
        .verdict = permission::Verdict::allow,
        .tool_pattern = std::string{tool::kToolSearchName},
    });
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    auto result =
        co_await registry.dispatch(tool::kToolSearchName, R"({"category":"memory","capability":"read_memory"})", ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text.contains("MemoryRecall [memory] [deferred]"));
    REQUIRE(result->data_json.has_value());
    const auto data = nlohmann::json::parse(*result->data_json);
    REQUIRE(data["query"]["category"] == "memory");
    REQUIRE(data["query"]["capability"] == "read_memory");
    REQUIRE(data["match_count"] == 1);
    REQUIRE(data["matches"][0]["name"] == "MemoryRecall");
    REQUIRE(data["matches"][0]["deferred"] == true);
    REQUIRE(data["matches"][0]["required_capabilities"] == nlohmann::json::array({"read_memory"}));
  });
}

TEST_CASE("ToolSearch reads the dispatching registry after Registry move", "[unit][tool][tool_search]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry original;
    REQUIRE(tool::register_tool_search(original).has_value());

    tool::Registry registry = std::move(original);
    REQUIRE(tool::register_file_read(registry).has_value());

    auto rules = single_rule(permission::Rule{
        .verdict = permission::Verdict::allow,
        .tool_pattern = std::string{tool::kToolSearchName},
    });
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    auto result = co_await registry.dispatch(tool::kToolSearchName, R"({"name":"FileRead"})", ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->data_json.has_value());
    const auto data = nlohmann::json::parse(*result->data_json);
    REQUIRE(data["match_count"] == 1);
    REQUIRE(data["matches"][0]["name"] == "FileRead");
  });
}

TEST_CASE("ToolSearch rejects malformed selectors as invalid_argument", "[unit][tool][tool_search]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_tool_search(registry).has_value());

    auto rules = single_rule(permission::Rule{
        .verdict = permission::Verdict::allow,
        .tool_pattern = std::string{tool::kToolSearchName},
    });
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    auto bad_json = co_await registry.dispatch(tool::kToolSearchName, "{not-json}", ctx);
    REQUIRE_FALSE(bad_json.has_value());
    REQUIRE(bad_json.error().kind() == core::ErrorKind::invalid_argument);

    auto non_object = co_await registry.dispatch(tool::kToolSearchName, "[]", ctx);
    REQUIRE_FALSE(non_object.has_value());
    REQUIRE(non_object.error().kind() == core::ErrorKind::invalid_argument);

    auto missing_selector = co_await registry.dispatch(tool::kToolSearchName, "{}", ctx);
    REQUIRE_FALSE(missing_selector.has_value());
    REQUIRE(missing_selector.error().kind() == core::ErrorKind::invalid_argument);

    auto wrong_type = co_await registry.dispatch(tool::kToolSearchName, R"({"name":42})", ctx);
    REQUIRE_FALSE(wrong_type.has_value());
    REQUIRE(wrong_type.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(context_has(wrong_type.error(), "field", "name"));

    auto unknown_capability = co_await registry.dispatch(tool::kToolSearchName, R"({"capability":"warp_drive"})", ctx);
    REQUIRE_FALSE(unknown_capability.has_value());
    REQUIRE(unknown_capability.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(context_has(unknown_capability.error(), "capability", "warp_drive"));

    REQUIRE(sink.events().size() == 5);
  });
}

TEST_CASE("MemoryRecall delegates parsed query through DispatchContext", "[unit][tool][memory_recall]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_memory_recall(registry).has_value());

    auto rules = single_rule(permission::Rule{
        .verdict = permission::Verdict::allow,
        .tool_pattern = std::string{tool::kMemoryRecallName},
        .capability = core::Capability::read_memory,
    });
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);
    auto seen = tool::MemoryRecallRequest{};
    ctx.memory_recall = [&seen](tool::MemoryRecallRequest request,
                                tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
      seen = std::move(request);
      co_return tool::Output{
          .text = "MemoryRecall: delegated",
          .data_json = R"({"kind":"memory_recall","match_count":1,"records":[]})",
          .usage = tool::ToolUsage{.match_count = 1},
      };
    };

    auto result = co_await registry.dispatch(tool::kMemoryRecallName,
                                             R"({"query":"scoped slices","limit":7,"kinds":["project","reference"]})",
                                             ctx);

    REQUIRE(result.has_value());
    REQUIRE(result->text == "MemoryRecall: delegated");
    REQUIRE(result->data_json.has_value());
    REQUIRE(result->usage.match_count.has_value());
    REQUIRE(*result->usage.match_count == 1);
    REQUIRE(seen.query == "scoped slices");
    REQUIRE(seen.limit == 7);
    REQUIRE(seen.kinds == std::vector<std::string>{"project", "reference"});
    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::allow);
  });
}

TEST_CASE("MemoryRecall reports missing runtime service as a model-repairable error", "[unit][tool][memory_recall]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_memory_recall(registry).has_value());

    auto rules = single_rule(permission::Rule{
        .verdict = permission::Verdict::allow,
        .tool_pattern = std::string{tool::kMemoryRecallName},
        .capability = core::Capability::read_memory,
    });
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    auto result = co_await registry.dispatch(tool::kMemoryRecallName, R"({"query":"scoped slices"})", ctx);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(context_has(result.error(), "reason", "memory_runtime_unavailable"));
    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::allow);
  });
}

TEST_CASE("MemoryRemember delegates parsed record fields through DispatchContext", "[unit][tool][memory_remember]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_memory_remember(registry).has_value());

    auto rules = single_rule(permission::Rule{
        .verdict = permission::Verdict::allow,
        .tool_pattern = std::string{tool::kMemoryRememberName},
        .capability = core::Capability::write_memory,
    });
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);
    auto seen = tool::MemoryRememberRequest{};
    ctx.memory_remember = [&seen](tool::MemoryRememberRequest request,
                                  tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
      seen = std::move(request);
      co_return tool::Output{
          .text = "MemoryRemember: delegated",
          .data_json = R"({"kind":"memory_remember","record":{"id":"rec-1"}})",
          .usage = tool::ToolUsage{.bytes_written = 42},
      };
    };

    auto result = co_await registry.dispatch(
        tool::kMemoryRememberName,
        R"({"id":"rec-1","kind":"project","title":"Build note","body":"Remember this.","importance":0.75,"tags":["repo","slice"],"linked_record_ids":["rec-0"],"shadow":true})",
        ctx);

    REQUIRE(result.has_value());
    REQUIRE(result->text == "MemoryRemember: delegated");
    REQUIRE(result->data_json.has_value());
    REQUIRE(result->usage.bytes_written.has_value());
    REQUIRE(*result->usage.bytes_written == 42);
    REQUIRE(seen.id == "rec-1");
    REQUIRE(seen.kind == "project");
    REQUIRE(seen.title == "Build note");
    REQUIRE(seen.body == "Remember this.");
    REQUIRE(seen.importance == 0.75);
    REQUIRE(seen.tags == std::vector<std::string>{"repo", "slice"});
    REQUIRE(seen.linked_record_ids == std::vector<std::string>{"rec-0"});
    REQUIRE(seen.shadow);
    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::allow);
  });
}

TEST_CASE("MemoryRemember reports missing runtime service as a model-repairable error",
          "[unit][tool][memory_remember]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_memory_remember(registry).has_value());

    auto rules = single_rule(permission::Rule{
        .verdict = permission::Verdict::allow,
        .tool_pattern = std::string{tool::kMemoryRememberName},
        .capability = core::Capability::write_memory,
    });
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    auto result =
        co_await registry.dispatch(tool::kMemoryRememberName,
                                   R"({"id":"rec-1","kind":"project","title":"Build note","body":"Remember this."})",
                                   ctx);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(context_has(result.error(), "reason", "memory_runtime_unavailable"));
    REQUIRE(context_has(result.error(), "id", "rec-1"));
    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::allow);
  });
}

TEST_CASE("MemoryRemember rejects malformed input as invalid_argument", "[unit][tool][memory_remember]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_memory_remember(registry).has_value());

    auto rules = single_rule(permission::Rule{
        .verdict = permission::Verdict::allow,
        .tool_pattern = std::string{tool::kMemoryRememberName},
        .capability = core::Capability::write_memory,
    });
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto malformed = std::vector<std::string_view>{
        "{",
        "[]",
        R"({"id":"rec-1","kind":"project","title":"Build note"})",
        R"({"id":"","kind":"project","title":"Build note","body":"Remember this."})",
        R"({"id":"rec-1","kind":"project","title":"Build note","body":"Remember this.","importance":"high"})",
        R"({"id":"rec-1","kind":"project","title":"Build note","body":"Remember this.","importance":1.25})",
        R"({"id":"rec-1","kind":"project","title":"Build note","body":"Remember this.","tags":["repo","repo"]})",
        R"({"id":"rec-1","kind":"project","title":"Build note","body":"Remember this.","linked_record_ids":[7]})",
        R"({"id":"rec-1","kind":"project","title":"Build note","body":"Remember this.","shadow":"no"})",
    };

    for (const auto input_json : malformed) {
      auto result = co_await registry.dispatch(tool::kMemoryRememberName, input_json, ctx);
      REQUIRE_FALSE(result.has_value());
      REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
    }
    REQUIRE(sink.events().size() == malformed.size());
  });
}

TEST_CASE("MemoryForget delegates parsed id through DispatchContext", "[unit][tool][memory_forget]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_memory_forget(registry).has_value());

    auto rules = single_rule(permission::Rule{
        .verdict = permission::Verdict::allow,
        .tool_pattern = std::string{tool::kMemoryForgetName},
        .capability = core::Capability::write_memory,
    });
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);
    auto seen = tool::MemoryForgetRequest{};
    ctx.memory_forget = [&seen](tool::MemoryForgetRequest request,
                                tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
      seen = std::move(request);
      co_return tool::Output{
          .text = "MemoryForget: delegated",
          .data_json = R"({"kind":"memory_forget","record":{"id":"rec-1"}})",
          .usage = tool::ToolUsage{.bytes_written = 0},
      };
    };

    auto result = co_await registry.dispatch(tool::kMemoryForgetName, R"({"id":"rec-1"})", ctx);

    REQUIRE(result.has_value());
    REQUIRE(result->text == "MemoryForget: delegated");
    REQUIRE(result->data_json.has_value());
    REQUIRE(result->usage.bytes_written.has_value());
    REQUIRE(*result->usage.bytes_written == 0);
    REQUIRE(seen.id == "rec-1");
    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::allow);
  });
}

TEST_CASE("MemoryForget reports missing runtime service as a model-repairable error", "[unit][tool][memory_forget]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_memory_forget(registry).has_value());

    auto rules = single_rule(permission::Rule{
        .verdict = permission::Verdict::allow,
        .tool_pattern = std::string{tool::kMemoryForgetName},
        .capability = core::Capability::write_memory,
    });
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    auto result = co_await registry.dispatch(tool::kMemoryForgetName, R"({"id":"rec-1"})", ctx);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(context_has(result.error(), "reason", "memory_runtime_unavailable"));
    REQUIRE(context_has(result.error(), "id", "rec-1"));
    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::allow);
  });
}

TEST_CASE("MemoryForget rejects malformed input as invalid_argument", "[unit][tool][memory_forget]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_memory_forget(registry).has_value());

    auto rules = single_rule(permission::Rule{
        .verdict = permission::Verdict::allow,
        .tool_pattern = std::string{tool::kMemoryForgetName},
        .capability = core::Capability::write_memory,
    });
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto malformed = std::vector<std::string_view>{
        "{",
        "[]",
        R"({})",
        R"({"id":7})",
        R"({"id":""})",
    };

    for (const auto input_json : malformed) {
      auto result = co_await registry.dispatch(tool::kMemoryForgetName, input_json, ctx);
      REQUIRE_FALSE(result.has_value());
      REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
    }
    REQUIRE(sink.events().size() == malformed.size());
  });
}

TEST_CASE("FileRead returns text fallback and structured metadata", "[unit][tool][file_read]") {
  TempFile file{"happy"};
  file.write("hello, slice 17");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_read(registry).has_value());
    auto rules = single_rule(permission::Rule{
        .verdict = permission::Verdict::allow,
        .tool_pattern = std::string{tool::kFileReadName},
        .capability = core::Capability::read_file,
    });
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto input = std::string{R"({"path":")"} + file.string() + R"("})";
    auto result = co_await registry.dispatch(tool::kFileReadName, input, ctx);
    REQUIRE(result.has_value());
    // v2 surface wraps the body in a `<path>:<start>-<end> fingerprint=<token>
    // bytes=<n>[ truncated]\n<body>` envelope; the legacy verbatim payload
    // is still available after the header line so callers can split on the
    // first newline and recover the original contents.
    const auto newline = result->text.find('\n');
    REQUIRE(newline != std::string::npos);
    const auto header = std::string_view{result->text}.substr(0, newline);
    const auto body = std::string_view{result->text}.substr(newline + 1);
    REQUIRE(body == "hello, slice 17");
    REQUIRE(header.starts_with(file.string() + ":1-1 fingerprint=v1:"));
    REQUIRE(header.contains("bytes=15"));
    REQUIRE_FALSE(header.contains(" truncated"));
    REQUIRE(result->data_json.has_value());
    const auto data = nlohmann::json::parse(*result->data_json);
    REQUIRE(data["kind"] == "file_read");
    REQUIRE(data["path"] == file.string());
    REQUIRE(data["text"] == "hello, slice 17");
    REQUIRE(data["fingerprint"].get<std::string>().starts_with("v1:"));
    REQUIRE(data["start_line"] == 1);
    REQUIRE(data["end_line"] == 1);
    REQUIRE(data["returned_bytes"] == 15);
    REQUIRE(data["truncated"] == false);
    REQUIRE(result->usage.bytes_read.has_value());
    REQUIRE(*result->usage.bytes_read == std::uintmax_t{15});
    REQUIRE(result->usage.files_touched.has_value());
    REQUIRE(*result->usage.files_touched == std::uint32_t{1});
    REQUIRE_FALSE(result->usage.truncated);
    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::allow);
  });
}

TEST_CASE("FileRead returns not_found when the path does not exist", "[unit][tool][file_read]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_read(registry).has_value());
    auto rules = single_rule(permission::Rule{
        .verdict = permission::Verdict::allow,
        .tool_pattern = std::string{tool::kFileReadName},
        .capability = core::Capability::read_file,
    });
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto input = std::string{R"({"path":"/tmp/oran-tool-this-path-does-not-exist-)"} +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + R"("})";
    auto result = co_await registry.dispatch(tool::kFileReadName, input, ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::not_found);
  });
}

namespace {

[[nodiscard]] std::pair<std::string_view, std::string_view> split_file_read_envelope(std::string_view text) {
  const auto newline = text.find('\n');
  REQUIRE(newline != std::string_view::npos);
  return {text.substr(0, newline), text.substr(newline + 1)};
}

[[nodiscard]] std::string extract_token(std::string_view header) {
  const auto fp = header.find("fingerprint=");
  REQUIRE(fp != std::string_view::npos);
  const auto rest = header.substr(fp + std::string_view{"fingerprint="}.size());
  const auto end = rest.find(' ');
  REQUIRE(end != std::string_view::npos);
  return std::string{rest.substr(0, end)};
}

}  // namespace

TEST_CASE("FileRead line range returns only the requested span", "[unit][tool][file_read][range]") {
  TempFile file{"line-range"};
  file.write("alpha\nbeta\ngamma\ndelta\nepsilon\n");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_read(registry).has_value());
    auto rules = single_rule(permission::Rule{
        .verdict = permission::Verdict::allow,
        .tool_pattern = std::string{tool::kFileReadName},
        .capability = core::Capability::read_file,
    });
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto input = std::format(R"({{"path":"{}","start_line":2,"line_count":2}})", file.string());
    auto result = co_await registry.dispatch(tool::kFileReadName, input, ctx);
    REQUIRE(result.has_value());
    auto [header, body] = split_file_read_envelope(result->text);
    REQUIRE(header.contains(":2-3 "));
    REQUIRE(body == "beta\ngamma\n");
    REQUIRE(result->data_json.has_value());
    const auto data = nlohmann::json::parse(*result->data_json);
    REQUIRE(data["path"] == file.string());
    REQUIRE(data["text"] == "beta\ngamma\n");
    REQUIRE(data["start_line"] == 2);
    REQUIRE(data["end_line"] == 3);
    REQUIRE(data["returned_bytes"] == 11);
    REQUIRE(data["truncated"] == false);
    REQUIRE(result->usage.bytes_read.has_value());
    REQUIRE(*result->usage.bytes_read == std::uintmax_t{11});
  });
}

TEST_CASE("FileRead byte range returns the requested byte span", "[unit][tool][file_read][range]") {
  TempFile file{"byte-range"};
  file.write("0123456789ABCDEF");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_read(registry).has_value());
    auto rules = single_rule(permission::Rule{
        .verdict = permission::Verdict::allow,
        .tool_pattern = std::string{tool::kFileReadName},
        .capability = core::Capability::read_file,
    });
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto input = std::format(R"({{"path":"{}","offset_bytes":3,"length_bytes":5}})", file.string());
    auto result = co_await registry.dispatch(tool::kFileReadName, input, ctx);
    REQUIRE(result.has_value());
    auto [header, body] = split_file_read_envelope(result->text);
    REQUIRE(header.contains("bytes=5"));
    // `offset_bytes` is the byte skip count from the start of the file — the
    // first read byte sits at zero-based index `offset_bytes`.
    REQUIRE(body == "34567");
    REQUIRE(result->data_json.has_value());
    const auto data = nlohmann::json::parse(*result->data_json);
    REQUIRE(data["text"] == "34567");
    REQUIRE(data["start_line"] == 1);
    REQUIRE(data["end_line"] == 0);
    REQUIRE(data["returned_bytes"] == 5);
    REQUIRE(data["truncated"] == false);
  });
}

TEST_CASE("FileRead max_bytes reports truncation in data_json and usage", "[unit][tool][file_read][range]") {
  TempFile file{"read-max-bytes"};
  file.write("0123456789");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_read(registry).has_value());
    auto rules = single_rule(permission::Rule{
        .verdict = permission::Verdict::allow,
        .tool_pattern = std::string{tool::kFileReadName},
        .capability = core::Capability::read_file,
    });
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto input = std::format(R"({{"path":"{}","max_bytes":4}})", file.string());
    auto result = co_await registry.dispatch(tool::kFileReadName, input, ctx);
    REQUIRE(result.has_value());
    auto [header, body] = split_file_read_envelope(result->text);
    REQUIRE(header.contains("bytes=4 truncated"));
    REQUIRE(body == "0123");
    REQUIRE(result->data_json.has_value());
    const auto data = nlohmann::json::parse(*result->data_json);
    REQUIRE(data["text"] == "0123");
    REQUIRE(data["returned_bytes"] == 4);
    REQUIRE(data["truncated"] == true);
    REQUIRE(result->usage.bytes_read.has_value());
    REQUIRE(*result->usage.bytes_read == std::uintmax_t{4});
    REQUIRE(result->usage.files_touched.has_value());
    REQUIRE(*result->usage.files_touched == std::uint32_t{1});
    REQUIRE(result->usage.truncated);
  });
}

TEST_CASE("FileRead rejects mixing line and byte range", "[unit][tool][file_read][range]") {
  TempFile file{"mixed-range"};
  file.write("contents");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_read(registry).has_value());
    auto rules = single_rule(permission::Rule{
        .verdict = permission::Verdict::allow,
        .tool_pattern = std::string{tool::kFileReadName},
        .capability = core::Capability::read_file,
    });
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto input = std::format(R"({{"path":"{}","start_line":1,"line_count":1,"offset_bytes":1,"length_bytes":1}})",
                                   file.string());
    auto result = co_await registry.dispatch(tool::kFileReadName, input, ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  });
}

TEST_CASE("FileRead if_version short-circuits unchanged files as not_modified", "[unit][tool][file_read][if_version]") {
  TempFile file{"if-version"};
  file.write("payload");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_read(registry).has_value());
    auto rules = single_rule(permission::Rule{
        .verdict = permission::Verdict::allow,
        .tool_pattern = std::string{tool::kFileReadName},
        .capability = core::Capability::read_file,
    });
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto base_input = std::format(R"({{"path":"{}"}})", file.string());
    auto first = co_await registry.dispatch(tool::kFileReadName, base_input, ctx);
    REQUIRE(first.has_value());
    auto [header, body] = split_file_read_envelope(first->text);
    REQUIRE(body == "payload");
    const auto token = extract_token(header);
    REQUIRE(token.starts_with("v1:"));

    // Same fingerprint on the follow-up read → `not_modified`.
    const auto cached_input = std::format(R"({{"path":"{}","if_version":"{}"}})", file.string(), token);
    auto cached = co_await registry.dispatch(tool::kFileReadName, cached_input, ctx);
    REQUIRE_FALSE(cached.has_value());
    REQUIRE(cached.error().kind() == core::ErrorKind::not_modified);
    REQUIRE(context_has(cached.error(), "fingerprint", token));

    // Stale token (one off) re-sends the body.
    const auto stale_token = std::string{"v1:0000000000000000000000000000000000000000000000000000000000000000:7:0"};
    const auto fresh_input = std::format(R"({{"path":"{}","if_version":"{}"}})", file.string(), stale_token);
    auto fresh = co_await registry.dispatch(tool::kFileReadName, fresh_input, ctx);
    REQUIRE(fresh.has_value());
    auto [hdr2, body2] = split_file_read_envelope(fresh->text);
    REQUIRE(body2 == "payload");
    REQUIRE(extract_token(hdr2) == token);
  });
}

TEST_CASE("FileRead version token changes when the file is rewritten", "[unit][tool][file_read][if_version]") {
  TempFile file{"if-version-changes"};
  file.write("first");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_read(registry).has_value());
    auto rules = single_rule(permission::Rule{
        .verdict = permission::Verdict::allow,
        .tool_pattern = std::string{tool::kFileReadName},
        .capability = core::Capability::read_file,
    });
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    auto first = co_await registry.dispatch(tool::kFileReadName, std::format(R"({{"path":"{}"}})", file.string()), ctx);
    REQUIRE(first.has_value());
    const auto first_token = extract_token(split_file_read_envelope(first->text).first);

    // Rewrite the file with a different body so size + mtime both shift.
    std::this_thread::sleep_for(std::chrono::milliseconds{15});
    file.write("rewritten-body");

    auto second =
        co_await registry.dispatch(tool::kFileReadName, std::format(R"({{"path":"{}"}})", file.string()), ctx);
    REQUIRE(second.has_value());
    const auto second_token = extract_token(split_file_read_envelope(second->text).first);
    REQUIRE(first_token != second_token);
  });
}

TEST_CASE("register_file_write advertises a `write_file` capability and a path/content schema",
          "[unit][tool][file_write]") {
  tool::Registry registry;
  REQUIRE(tool::register_file_write(registry).has_value());
  REQUIRE(registry.size() == 1);
  const auto* def = registry.find(tool::kFileWriteName);
  REQUIRE(def != nullptr);
  REQUIRE(def->required_capabilities.size() == 1);
  REQUIRE(def->required_capabilities[0] == core::Capability::write_file);
  REQUIRE(def->input_schema_json.contains("\"path\""));
  REQUIRE(def->input_schema_json.contains("\"content\""));
  REQUIRE(def->input_schema_json.contains("\"mode\""));
  REQUIRE(def->input_schema_json.contains("\"create_parents\""));
  REQUIRE(def->input_schema_json.contains("\"max_bytes\""));
}

namespace {

[[nodiscard]] std::string slurp(const std::string& path) {
  std::ifstream input{path, std::ios::binary};
  return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

permission::RuleSet write_rule_set() {
  return single_rule(permission::Rule{
      .verdict = permission::Verdict::allow,
      .tool_pattern = std::string{tool::kFileWriteName},
      .capability = core::Capability::write_file,
  });
}

}  // namespace

TEST_CASE("FileWrite requires a workspace authority", "[unit][tool][file_write][workspace]") {
  TempFile file{"write-no-workspace"};
  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_write(registry).has_value());
    auto rules = write_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto input = std::format(R"({{"path":"{}","content":"blocked"}})", file.string());
    auto result = co_await registry.dispatch(tool::kFileWriteName, input, ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::internal);
    REQUIRE_FALSE(std::filesystem::exists(file.string()));
  });
}

TEST_CASE("FileWrite happy path writes the bytes verbatim and reports the size", "[unit][tool][file_write]") {
  TempFile file{"happy-write"};

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_write(registry).has_value());
    auto rules = write_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_temp_workspace_ctx(io, rules, sink, permission::Mode::strict);

    nlohmann::json input{{"path", file.string()}, {"content", "hello, slice 18"}};
    auto result = co_await registry.dispatch(tool::kFileWriteName, input.dump(), ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text.contains("wrote 15 bytes"));
    REQUIRE(result->text.contains(file.string()));
    REQUIRE(result->usage.bytes_written.has_value());
    REQUIRE(*result->usage.bytes_written == std::uintmax_t{15});
    REQUIRE_FALSE(result->usage.bytes_read.has_value());
    REQUIRE(result->usage.files_touched.has_value());
    REQUIRE(*result->usage.files_touched == std::uint32_t{1});
    REQUIRE_FALSE(result->data_json.has_value());
    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::allow);
  });

  REQUIRE(slurp(file.string()) == "hello, slice 18");
}

TEST_CASE("FileWrite default mode overwrites an existing file", "[unit][tool][file_write]") {
  TempFile file{"overwrite"};
  file.write("original");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_write(registry).has_value());
    auto rules = write_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_temp_workspace_ctx(io, rules, sink, permission::Mode::strict);

    nlohmann::json input{{"path", file.string()}, {"content", "replaced"}};
    auto result = co_await registry.dispatch(tool::kFileWriteName, input.dump(), ctx);
    REQUIRE(result.has_value());
  });

  REQUIRE(slurp(file.string()) == "replaced");
}

TEST_CASE("FileWrite mode=append appends to existing content", "[unit][tool][file_write]") {
  TempFile file{"append"};
  file.write("head");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_write(registry).has_value());
    auto rules = write_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_temp_workspace_ctx(io, rules, sink, permission::Mode::strict);

    nlohmann::json input{{"path", file.string()}, {"content", "-tail"}, {"mode", "append"}};
    auto result = co_await registry.dispatch(tool::kFileWriteName, input.dump(), ctx);
    REQUIRE(result.has_value());
  });

  REQUIRE(slurp(file.string()) == "head-tail");
}

TEST_CASE("FileWrite mode=fail_if_exists returns conflict when the path already exists", "[unit][tool][file_write]") {
  TempFile file{"exists"};
  file.write("present");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_write(registry).has_value());
    auto rules = write_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_temp_workspace_ctx(io, rules, sink, permission::Mode::strict);

    nlohmann::json input{{"path", file.string()}, {"content", "replacement"}, {"mode", "fail_if_exists"}};
    auto result = co_await registry.dispatch(tool::kFileWriteName, input.dump(), ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::conflict);
  });

  REQUIRE(slurp(file.string()) == "present");
}

TEST_CASE("FileWrite create_parents=true creates missing directories", "[unit][tool][file_write]") {
  const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const auto base = std::filesystem::temp_directory_path() / ("oran-tool-create-parents-" + stamp);
  const auto target = base / "nested" / "deeper" / "out.txt";

  test::run_async([&target](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_write(registry).has_value());
    auto rules = write_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_temp_workspace_ctx(io, rules, sink, permission::Mode::strict);

    nlohmann::json input{{"path", target.string()}, {"content", "deep"}, {"create_parents", true}};
    auto result = co_await registry.dispatch(tool::kFileWriteName, input.dump(), ctx);
    REQUIRE(result.has_value());
  });

  REQUIRE(std::filesystem::exists(target));
  REQUIRE(slurp(target.string()) == "deep");

  std::error_code ec;
  std::filesystem::remove_all(base, ec);
}

TEST_CASE("FileWrite enforces max_bytes and leaves existing content untouched", "[unit][tool][file_write][max_bytes]") {
  TempFile file{"write-max-bytes"};
  file.write("original");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_write(registry).has_value());
    auto rules = write_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_temp_workspace_ctx(io, rules, sink, permission::Mode::strict);

    nlohmann::json boundary{{"path", file.string()}, {"content", "1234"}, {"max_bytes", 4}};
    auto boundary_result = co_await registry.dispatch(tool::kFileWriteName, boundary.dump(), ctx);
    REQUIRE(boundary_result.has_value());

    nlohmann::json oversized{{"path", file.string()}, {"content", "12345"}, {"max_bytes", 4}};
    auto result = co_await registry.dispatch(tool::kFileWriteName, oversized.dump(), ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(context_has(result.error(), "content_bytes", "5"));
    REQUIRE(context_has(result.error(), "max_bytes", "4"));
    REQUIRE(sink.events().size() == 2);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::allow);
    REQUIRE(sink.events()[1].outcome == permission::AuditOutcome::deny);
  });

  REQUIRE(slurp(file.string()) == "1234");
}

namespace {

permission::RuleSet read_and_write_rule_set() {
  permission::RuleSet rs;
  rs.push_back(permission::Rule{
      .verdict = permission::Verdict::allow,
      .tool_pattern = std::string{tool::kFileReadName},
      .capability = core::Capability::read_file,
  });
  rs.push_back(permission::Rule{
      .verdict = permission::Verdict::allow,
      .tool_pattern = std::string{tool::kFileWriteName},
      .capability = core::Capability::write_file,
  });
  return rs;
}

permission::RuleSet read_and_edit_rule_set() {
  permission::RuleSet rs;
  rs.push_back(permission::Rule{
      .verdict = permission::Verdict::allow,
      .tool_pattern = std::string{tool::kFileReadName},
      .capability = core::Capability::read_file,
  });
  rs.push_back(permission::Rule{
      .verdict = permission::Verdict::allow,
      .tool_pattern = std::string{tool::kFileEditName},
      .capability = core::Capability::edit_file,
  });
  return rs;
}

[[nodiscard]] async::Awaitable<std::string>
dispatch_read_and_extract_token(tool::Registry& registry, tool::DispatchContext& ctx, std::string_view path) {
  auto read = co_await registry.dispatch(tool::kFileReadName, std::format(R"({{"path":"{}"}})", path), ctx);
  REQUIRE(read.has_value());
  const auto newline = read->text.find('\n');
  REQUIRE(newline != std::string::npos);
  const auto header = std::string_view{read->text}.substr(0, newline);
  co_return std::string{extract_token(header)};
}

}  // namespace

TEST_CASE("FileWrite expected_version succeeds when the token matches", "[unit][tool][file_write][if_version]") {
  TempFile file{"write-expected-ok"};
  file.write("seed");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_read(registry).has_value());
    REQUIRE(tool::register_file_write(registry).has_value());
    auto rules = read_and_write_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_temp_workspace_ctx(io, rules, sink, permission::Mode::strict);

    const auto token = co_await dispatch_read_and_extract_token(registry, ctx, file.string());
    const auto input =
        std::format(R"({{"path":"{}","content":"replaced","expected_version":"{}"}})", file.string(), token);
    auto written = co_await registry.dispatch(tool::kFileWriteName, input, ctx);
    REQUIRE(written.has_value());
    REQUIRE(slurp(file.string()) == "replaced");
  });
}

TEST_CASE("FileWrite expected_version returns conflict when the token is stale",
          "[unit][tool][file_write][if_version]") {
  TempFile file{"write-expected-stale"};
  file.write("seed");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_read(registry).has_value());
    REQUIRE(tool::register_file_write(registry).has_value());
    auto rules = read_and_write_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_temp_workspace_ctx(io, rules, sink, permission::Mode::strict);

    const auto stale = std::string{"v1:0000000000000000000000000000000000000000000000000000000000000000:4:0"};
    const auto input =
        std::format(R"({{"path":"{}","content":"clobber","expected_version":"{}"}})", file.string(), stale);
    auto attempt = co_await registry.dispatch(tool::kFileWriteName, input, ctx);
    REQUIRE_FALSE(attempt.has_value());
    REQUIRE(attempt.error().kind() == core::ErrorKind::conflict);
    REQUIRE(context_has(attempt.error(), "reason", "stale_fingerprint"));
    REQUIRE(context_has(attempt.error(), "expected", stale));
    // The current fingerprint travels in the error so the agent can re-read
    // with the fresh token in the next turn.
    const auto& ctx_entries = attempt.error().context();
    const auto fp = std::ranges::find_if(ctx_entries, [](const auto& e) { return e.first == "fingerprint"; });
    REQUIRE(fp != ctx_entries.end());
    REQUIRE(std::string_view{fp->second}.starts_with("v1:"));
    REQUIRE(slurp(file.string()) == "seed");
  });
}

TEST_CASE("FileWrite expected_version on a missing file surfaces conflict", "[unit][tool][file_write][if_version]") {
  TempFile file{"write-expected-missing"};  // deliberately not created
  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_write(registry).has_value());
    auto rules = write_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_temp_workspace_ctx(io, rules, sink, permission::Mode::strict);

    const auto stale = std::string{"v1:0000000000000000000000000000000000000000000000000000000000000000:0:0"};
    const auto input = std::format(R"({{"path":"{}","content":"new","expected_version":"{}"}})", file.string(), stale);
    auto attempt = co_await registry.dispatch(tool::kFileWriteName, input, ctx);
    REQUIRE_FALSE(attempt.has_value());
    REQUIRE(attempt.error().kind() == core::ErrorKind::conflict);
    REQUIRE(context_has(attempt.error(), "reason", "stale_fingerprint"));
  });
}

TEST_CASE("register_file_edit advertises an `edit_file` capability and a path/old/new schema",
          "[unit][tool][file_edit]") {
  tool::Registry registry;
  REQUIRE(tool::register_file_edit(registry).has_value());
  REQUIRE(registry.size() == 1);
  const auto* def = registry.find(tool::kFileEditName);
  REQUIRE(def != nullptr);
  REQUIRE(def->required_capabilities.size() == 1);
  REQUIRE(def->required_capabilities[0] == core::Capability::edit_file);
  REQUIRE(def->input_schema_json.contains("\"path\""));
  REQUIRE(def->input_schema_json.contains("\"old_string\""));
  REQUIRE(def->input_schema_json.contains("\"new_string\""));
  REQUIRE(def->input_schema_json.contains("\"replace_all\""));
  REQUIRE(def->input_schema_json.contains("\"max_bytes\""));
}

namespace {

permission::RuleSet edit_rule_set() {
  return single_rule(permission::Rule{
      .verdict = permission::Verdict::allow,
      .tool_pattern = std::string{tool::kFileEditName},
      .capability = core::Capability::edit_file,
  });
}

}  // namespace

TEST_CASE("FileEdit requires a workspace authority", "[unit][tool][file_edit][workspace]") {
  TempFile file{"edit-no-workspace"};
  file.write("alpha beta gamma");
  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_edit(registry).has_value());
    auto rules = edit_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto input = std::format(R"({{"path":"{}","old_string":"beta","new_string":"BETA"}})", file.string());
    auto result = co_await registry.dispatch(tool::kFileEditName, input, ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::internal);
    REQUIRE(slurp(file.string()) == "alpha beta gamma");
  });
}

TEST_CASE("FileEdit happy path replaces a unique occurrence and reports a count", "[unit][tool][file_edit]") {
  TempFile file{"edit-unique"};
  file.write("alpha beta gamma");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_edit(registry).has_value());
    auto rules = edit_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_temp_workspace_ctx(io, rules, sink, permission::Mode::strict);

    nlohmann::json input{{"path", file.string()}, {"old_string", "beta"}, {"new_string", "BETA"}};
    auto result = co_await registry.dispatch(tool::kFileEditName, input.dump(), ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text.contains("1 replacement"));
    REQUIRE_FALSE(result->text.contains("replacements"));
    REQUIRE(result->text.contains(file.string()));
    REQUIRE(result->usage.bytes_read.has_value());
    REQUIRE(*result->usage.bytes_read == std::uintmax_t{16});
    REQUIRE(result->usage.bytes_written.has_value());
    REQUIRE(*result->usage.bytes_written == std::uintmax_t{16});
    REQUIRE(result->usage.files_touched.has_value());
    REQUIRE(*result->usage.files_touched == std::uint32_t{1});
    REQUIRE(result->usage.match_count.has_value());
    REQUIRE(*result->usage.match_count == std::uint64_t{1});
    REQUIRE_FALSE(result->data_json.has_value());
    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::allow);
  });

  REQUIRE(slurp(file.string()) == "alpha BETA gamma");
}

TEST_CASE("FileEdit replace_all=true rewrites every occurrence", "[unit][tool][file_edit]") {
  TempFile file{"edit-replace-all"};
  file.write("foo bar foo baz foo");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_edit(registry).has_value());
    auto rules = edit_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_temp_workspace_ctx(io, rules, sink, permission::Mode::strict);

    nlohmann::json input{{"path", file.string()}, {"old_string", "foo"}, {"new_string", "qux"}, {"replace_all", true}};
    auto result = co_await registry.dispatch(tool::kFileEditName, input.dump(), ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text.contains("3 replacements"));
    REQUIRE(result->usage.bytes_read.has_value());
    REQUIRE(*result->usage.bytes_read == std::uintmax_t{19});
    REQUIRE(result->usage.bytes_written.has_value());
    REQUIRE(*result->usage.bytes_written == std::uintmax_t{19});
    REQUIRE(result->usage.files_touched.has_value());
    REQUIRE(*result->usage.files_touched == std::uint32_t{1});
    REQUIRE(result->usage.match_count.has_value());
    REQUIRE(*result->usage.match_count == std::uint64_t{3});
    REQUIRE_FALSE(result->data_json.has_value());
  });

  REQUIRE(slurp(file.string()) == "qux bar qux baz qux");
}

TEST_CASE("FileEdit returns conflict when old_string is not unique and replace_all is false",
          "[unit][tool][file_edit]") {
  TempFile file{"edit-ambiguous"};
  file.write("dup dup dup");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_edit(registry).has_value());
    auto rules = edit_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_temp_workspace_ctx(io, rules, sink, permission::Mode::strict);

    nlohmann::json input{{"path", file.string()}, {"old_string", "dup"}, {"new_string", "x"}};
    auto result = co_await registry.dispatch(tool::kFileEditName, input.dump(), ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::conflict);
    REQUIRE(context_has(result.error(), "match_count", "3"));
  });

  // File must be untouched when the call refuses.
  REQUIRE(slurp(file.string()) == "dup dup dup");
}

TEST_CASE("FileEdit returns not_found when old_string does not appear", "[unit][tool][file_edit]") {
  TempFile file{"edit-missing-substring"};
  file.write("hello world");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_edit(registry).has_value());
    auto rules = edit_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_temp_workspace_ctx(io, rules, sink, permission::Mode::strict);

    nlohmann::json input{{"path", file.string()}, {"old_string", "absent"}, {"new_string", "present"}};
    auto result = co_await registry.dispatch(tool::kFileEditName, input.dump(), ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::not_found);
  });

  REQUIRE(slurp(file.string()) == "hello world");
}

TEST_CASE("FileEdit enforces max_bytes on replacement output and leaves the file untouched",
          "[unit][tool][file_edit][max_bytes]") {
  TempFile file{"edit-max-bytes"};
  file.write("a b c");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_edit(registry).has_value());
    auto rules = edit_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_temp_workspace_ctx(io, rules, sink, permission::Mode::strict);

    nlohmann::json input{
        {"path", file.string()},
        {"old_string", "b"},
        {"new_string", "bbbb"},
        {"max_bytes", 5},
    };
    auto result = co_await registry.dispatch(tool::kFileEditName, input.dump(), ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(context_has(result.error(), "output_bytes", "8"));
    REQUIRE(context_has(result.error(), "max_bytes", "5"));
    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::allow);
  });

  REQUIRE(slurp(file.string()) == "a b c");
}

TEST_CASE("FileEdit propagates not_found when the file is missing", "[unit][tool][file_edit]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_edit(registry).has_value());
    auto rules = edit_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_temp_workspace_ctx(io, rules, sink, permission::Mode::strict);

    const auto path = std::string{"/tmp/oran-tool-edit-missing-"} +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    nlohmann::json input{{"path", path}, {"old_string", "x"}, {"new_string", "y"}};
    auto result = co_await registry.dispatch(tool::kFileEditName, input.dump(), ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::not_found);
  });
}

TEST_CASE("FileEdit expected_version succeeds when the token matches", "[unit][tool][file_edit][if_version]") {
  TempFile file{"edit-expected-ok"};
  file.write("alpha-beta-gamma");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_read(registry).has_value());
    REQUIRE(tool::register_file_edit(registry).has_value());
    auto rules = read_and_edit_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_temp_workspace_ctx(io, rules, sink, permission::Mode::strict);

    const auto token = co_await dispatch_read_and_extract_token(registry, ctx, file.string());
    const auto input = std::format(R"({{"path":"{}","old_string":"beta","new_string":"BETA","expected_version":"{}"}})",
                                   file.string(),
                                   token);
    auto edited = co_await registry.dispatch(tool::kFileEditName, input, ctx);
    REQUIRE(edited.has_value());
    REQUIRE(slurp(file.string()) == "alpha-BETA-gamma");
  });
}

TEST_CASE("FileEdit expected_version returns conflict when the token is stale", "[unit][tool][file_edit][if_version]") {
  TempFile file{"edit-expected-stale"};
  file.write("alpha-beta-gamma");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_edit(registry).has_value());
    auto rules = edit_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_temp_workspace_ctx(io, rules, sink, permission::Mode::strict);

    const auto stale = std::string{"v1:0000000000000000000000000000000000000000000000000000000000000000:16:0"};
    const auto input = std::format(R"({{"path":"{}","old_string":"beta","new_string":"BETA","expected_version":"{}"}})",
                                   file.string(),
                                   stale);
    auto attempt = co_await registry.dispatch(tool::kFileEditName, input, ctx);
    REQUIRE_FALSE(attempt.has_value());
    REQUIRE(attempt.error().kind() == core::ErrorKind::conflict);
    REQUIRE(context_has(attempt.error(), "reason", "stale_fingerprint"));
    REQUIRE(context_has(attempt.error(), "expected", stale));
    const auto& ctx_entries = attempt.error().context();
    const auto fp = std::ranges::find_if(ctx_entries, [](const auto& e) { return e.first == "fingerprint"; });
    REQUIRE(fp != ctx_entries.end());
    REQUIRE(std::string_view{fp->second}.starts_with("v1:"));
    // File untouched.
    REQUIRE(slurp(file.string()) == "alpha-beta-gamma");
  });
}

// ---------------------------------------------------------------------------
// Slice 21 — approval-broker dispatch wiring.
//
// The cases below exercise the new `ctx.approval_broker` + `ctx.approval_token`
// plumbing: when both are present and the rule fires `Verdict::ask`,
// `Registry::dispatch` consults the broker and promotes/demotes the audit
// outcome to `approved`/`rejected` instead of short-circuiting with
// `approval_required`. The legacy short-circuit path is still covered by
// the cases above so the partial-wiring (broker but no token, or no broker)
// transitions stay legible from this file alone.

namespace {

[[nodiscard]] permission::ApprovalBroker make_broker() {
  auto broker = permission::ApprovalBroker::with_random_secret();
  REQUIRE(broker.has_value());
  return std::move(*broker);
}

[[nodiscard]] core::Time fixed_now() noexcept {
  using namespace std::chrono;
  return core::Time{sys_days{year{2026} / January / day{1}}};
}

[[nodiscard]] permission::ApprovalToken grant(permission::ApprovalBroker& broker,
                                              std::string_view tool_name,
                                              std::string_view input,
                                              std::string_view identity,
                                              core::Time now,
                                              std::uint32_t replay_max = 4) {
  return broker.approve(
      permission::ApprovalGrant{
          .tool_name = tool_name,
          .input = input,
          .identity = identity,
          .ttl = std::chrono::seconds{60},
          .replay_max = replay_max,
      },
      now);
}

[[nodiscard]] tool::DispatchContext make_approval_ctx(asio::io_context& io,
                                                      permission::RuleSet& rules,
                                                      permission::AuditSink& sink,
                                                      permission::ApprovalBroker* broker,
                                                      const permission::ApprovalToken* token,
                                                      core::Time now,
                                                      permission::Mode mode = permission::Mode::default_) {
  return tool::DispatchContext{
      .executor = io.get_executor(),
      .mode = mode,
      .rules = rules,
      .audit = sink,
      .approval_broker = broker,
      .approval_token = token,
      .now = now,
      .scope_key = "scope-A",
      .agent_key = "coder",
      .identity = "operator-1",
  };
}

}  // namespace

TEST_CASE("Registry::dispatch routes ask through the broker on a valid token (audit outcome=approved)",
          "[unit][tool][registry][approval]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(core::ToolDef::with_no_input("noop", "noop"), make_echo_handler()).has_value());
    auto rules = single_rule(permission::Rule{.verdict = permission::Verdict::ask, .tool_pattern = "noop"});
    permission::RecordingAuditSink sink;
    auto broker = make_broker();
    const auto now = fixed_now();
    const std::string_view input = R"({"hello":"world"})";
    const auto token = grant(broker, "noop", input, "operator-1", now);

    auto ctx = make_approval_ctx(io, rules, sink, &broker, &token, now);
    auto result = co_await registry.dispatch("noop", input, ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text == input);

    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].verdict == permission::Verdict::ask);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::approved);
    // The rule reason survives on the approved path: the broker only swaps
    // the reason on a rejection.
    REQUIRE(sink.events()[0].reason == "rule #0 (ask: noop)");
  });
}

TEST_CASE("Registry::dispatch records rejected and forwards reason=replay_exhausted when the broker rejects",
          "[unit][tool][registry][approval]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(core::ToolDef::with_no_input("noop", "noop"), make_echo_handler()).has_value());
    auto rules = single_rule(permission::Rule{.verdict = permission::Verdict::ask, .tool_pattern = "noop"});
    permission::RecordingAuditSink sink;
    auto broker = make_broker();
    const auto now = fixed_now();
    const std::string_view input = R"({"x":1})";
    const auto token = grant(broker, "noop", input, "operator-1", now, /*replay_max=*/0);

    auto ctx = make_approval_ctx(io, rules, sink, &broker, &token, now);
    auto result = co_await registry.dispatch("noop", input, ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(context_has(result.error(), "reason", "replay_exhausted"));
    REQUIRE(context_has(result.error(), "tool", "noop"));

    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].verdict == permission::Verdict::ask);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::rejected);
    REQUIRE(sink.events()[0].reason == "replay_exhausted");
  });
}

TEST_CASE("Registry::dispatch records rejected with reason=no_grant when the broker has no entry",
          "[unit][tool][registry][approval]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(core::ToolDef::with_no_input("noop", "noop"), make_echo_handler()).has_value());
    auto rules = single_rule(permission::Rule{.verdict = permission::Verdict::ask, .tool_pattern = "noop"});
    permission::RecordingAuditSink sink;
    auto broker = make_broker();
    const auto now = fixed_now();
    const std::string_view input = R"({"x":1})";
    const auto token = grant(broker, "noop", input, "operator-1", now);
    // Reap the broker's map so the token verifies but no entry exists.
    broker.reap_expired(core::Time{now.to_system_time_point() + std::chrono::hours{2}});
    REQUIRE(broker.outstanding_grants() == 0);

    auto ctx = make_approval_ctx(io, rules, sink, &broker, &token, now);
    auto result = co_await registry.dispatch("noop", input, ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(context_has(result.error(), "reason", "no_grant"));

    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::rejected);
    REQUIRE(sink.events()[0].reason == "no_grant");
  });
}

TEST_CASE("Registry::dispatch records rejected with reason=expired when the token TTL has elapsed",
          "[unit][tool][registry][approval]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(core::ToolDef::with_no_input("noop", "noop"), make_echo_handler()).has_value());
    auto rules = single_rule(permission::Rule{.verdict = permission::Verdict::ask, .tool_pattern = "noop"});
    permission::RecordingAuditSink sink;
    auto broker = make_broker();
    const auto now = fixed_now();
    const std::string_view input = R"({"x":1})";
    // Issue at `now` with the default 60s TTL the helper uses, then dispatch
    // at `now + 2h` so the authority-level expiry kicks in.
    const auto token = grant(broker, "noop", input, "operator-1", now);
    const auto future = core::Time{now.to_system_time_point() + std::chrono::hours{2}};

    auto ctx = make_approval_ctx(io, rules, sink, &broker, &token, future);
    auto result = co_await registry.dispatch("noop", input, ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(context_has(result.error(), "reason", "expired"));

    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::rejected);
    REQUIRE(sink.events()[0].reason == "expired");
  });
}

TEST_CASE("Registry::dispatch records rejected with reason=tool_mismatch on cross-tool replay",
          "[unit][tool][registry][approval]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(core::ToolDef::with_no_input("alpha", "alpha"), make_echo_handler()).has_value());
    REQUIRE(registry.add(core::ToolDef::with_no_input("beta", "beta"), make_echo_handler()).has_value());

    auto rules = single_rule(permission::Rule{.verdict = permission::Verdict::ask, .tool_pattern = "*"});
    permission::RecordingAuditSink sink;
    auto broker = make_broker();
    const auto now = fixed_now();
    const std::string_view input = R"({"x":1})";
    // Token is issued for `alpha`; we present it during a `beta` dispatch.
    const auto token = grant(broker, "alpha", input, "operator-1", now);

    auto ctx = make_approval_ctx(io, rules, sink, &broker, &token, now);
    auto result = co_await registry.dispatch("beta", input, ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(context_has(result.error(), "reason", "tool_mismatch"));
    REQUIRE(context_has(result.error(), "tool", "beta"));

    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].tool_name == "beta");
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::rejected);
    REQUIRE(sink.events()[0].reason == "tool_mismatch");
  });
}

TEST_CASE("Registry::dispatch with broker but no token keeps the short-circuit approval_required path",
          "[unit][tool][registry][approval]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(core::ToolDef::with_no_input("noop", "noop"), make_echo_handler()).has_value());
    auto rules = single_rule(permission::Rule{.verdict = permission::Verdict::ask, .tool_pattern = "noop"});
    permission::RecordingAuditSink sink;
    auto broker = make_broker();
    auto ctx = make_approval_ctx(io, rules, sink, &broker, /*token=*/nullptr, fixed_now());

    auto result = co_await registry.dispatch("noop", "{}", ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(context_has(result.error(), "reason", "approval_required"));
    REQUIRE(context_has(result.error(), "replay_max", "8"));
    REQUIRE(context_has(result.error(), "approval_ttl_seconds", "3600"));

    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::ask);
    // The broker was not consulted; outstanding grants should still be zero.
    REQUIRE(broker.outstanding_grants() == 0);
  });
}

TEST_CASE("Registry::dispatch with broker and bus but no ask sink keeps approval_required",
          "[unit][tool][registry][approval][hook]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(core::ToolDef::with_no_input("noop", "noop"), make_echo_handler()).has_value());
    auto rules = single_rule(permission::Rule{.verdict = permission::Verdict::ask, .tool_pattern = "noop"});
    permission::RecordingAuditSink sink;
    auto broker = make_broker();
    orangutan::hook::Bus bus;
    auto ctx = make_approval_ctx(io, rules, sink, &broker, /*token=*/nullptr, fixed_now());
    ctx.bus = &bus;

    auto result = co_await registry.dispatch("noop", "{}", ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(context_has(result.error(), "reason", "approval_required"));
    REQUIRE(broker.outstanding_grants() == 0);

    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::ask);
    auto metadata = nlohmann::json::parse(sink.events()[0].metadata_json);
    REQUIRE_FALSE(metadata.contains("permission_ask_decisions"));
  });
}

TEST_CASE("Registry::dispatch does not consult the broker on allow verdicts (token ignored)",
          "[unit][tool][registry][approval]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(core::ToolDef::with_no_input("noop", "noop"), make_echo_handler()).has_value());
    auto rules = single_rule(permission::Rule{.verdict = permission::Verdict::allow, .tool_pattern = "noop"});
    permission::RecordingAuditSink sink;
    auto broker = make_broker();
    const auto now = fixed_now();
    // A token that is invalid for THIS call (`replay_max=0` means
    // `broker.check` would reject) — but verdict=allow short-circuits
    // before the broker is consulted, so the handler still runs.
    const std::string_view input = R"({"x":1})";
    const auto token = grant(broker, "noop", input, "operator-1", now, /*replay_max=*/0);

    auto ctx = make_approval_ctx(io, rules, sink, &broker, &token, now);
    auto result = co_await registry.dispatch("noop", input, ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text == input);

    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].verdict == permission::Verdict::allow);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::allow);
  });
}

TEST_CASE("Registry::dispatch does not consult the broker on deny verdicts (token ignored)",
          "[unit][tool][registry][approval]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(core::ToolDef::with_no_input("noop", "noop"), make_echo_handler()).has_value());
    auto rules = single_rule(permission::Rule{.verdict = permission::Verdict::deny, .tool_pattern = "noop"});
    permission::RecordingAuditSink sink;
    auto broker = make_broker();
    const auto now = fixed_now();
    // A valid token presented against a deny verdict — the deny path
    // ignores the token, returns permission_denied with the rule reason,
    // and records outcome=deny.
    const std::string_view input = R"({"x":1})";
    const auto token = grant(broker, "noop", input, "operator-1", now);

    auto ctx = make_approval_ctx(io, rules, sink, &broker, &token, now);
    auto result = co_await registry.dispatch("noop", input, ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE_FALSE(context_has(result.error(), "reason", "approval_required"));

    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].verdict == permission::Verdict::deny);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::deny);
  });
}

TEST_CASE("Registry::dispatch end-to-end: ask short-circuits, agent approves, re-dispatch with token succeeds",
          "[unit][tool][registry][approval]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(core::ToolDef::with_no_input("noop", "noop"), make_echo_handler()).has_value());
    auto rules = single_rule(permission::Rule{.verdict = permission::Verdict::ask,
                                              .tool_pattern = "noop",
                                              .replay_max = 3,
                                              .approval_ttl = std::chrono::seconds{120}});
    permission::RecordingAuditSink sink;
    auto broker = make_broker();
    const auto now = fixed_now();
    const std::string_view input = R"({"work":"unit"})";

    // First call mirrors the agent loop's "ask" turn: broker is supplied
    // (so the agent will be able to honor an upcoming approval), but no
    // token has been issued yet.
    auto first_ctx = make_approval_ctx(io, rules, sink, &broker, /*token=*/nullptr, now);
    auto first = co_await registry.dispatch("noop", input, first_ctx);
    REQUIRE_FALSE(first.has_value());
    REQUIRE(context_has(first.error(), "reason", "approval_required"));
    REQUIRE(context_has(first.error(), "replay_max", "3"));
    REQUIRE(context_has(first.error(), "approval_ttl_seconds", "120"));
    REQUIRE(sink.events().back().outcome == permission::AuditOutcome::ask);

    // The agent loop now hands the replay_max + ttl to broker.approve(),
    // capturing the resulting token and re-dispatching.
    const auto token = grant(broker, "noop", input, "operator-1", now, /*replay_max=*/3);
    auto second_ctx = make_approval_ctx(io, rules, sink, &broker, &token, now);
    auto second = co_await registry.dispatch("noop", input, second_ctx);
    REQUIRE(second.has_value());
    REQUIRE(second->text == input);
    REQUIRE(sink.events().back().outcome == permission::AuditOutcome::approved);

    // And the replay budget still allows one more re-use before exhausting.
    auto third = co_await registry.dispatch("noop", input, second_ctx);
    REQUIRE(third.has_value());
    auto fourth = co_await registry.dispatch("noop", input, second_ctx);
    REQUIRE(fourth.has_value());
    auto fifth = co_await registry.dispatch("noop", input, second_ctx);
    REQUIRE_FALSE(fifth.has_value());
    REQUIRE(context_has(fifth.error(), "reason", "replay_exhausted"));

    // Total audit rows: 1 ask + 3 approved + 1 rejected.
    REQUIRE(sink.events().size() == 5);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::ask);
    REQUIRE(sink.events()[1].outcome == permission::AuditOutcome::approved);
    REQUIRE(sink.events()[2].outcome == permission::AuditOutcome::approved);
    REQUIRE(sink.events()[3].outcome == permission::AuditOutcome::approved);
    REQUIRE(sink.events()[4].outcome == permission::AuditOutcome::rejected);
  });
}

// ---------------------------------------------------------------------------
// slice 22 — hook bus wiring into `Registry::dispatch`.
//
// The bus is optional on `DispatchContext`. When null, the existing behavior
// (slices 17..21) is preserved verbatim. When non-null, dispatch publishes
// `tool_before` once the tool def is resolved and `tool_after` at every
// exit (handler success, permission deny, broker rejection, audit error).
// Sinks are advisory in slice 22 — their errors are recorded by the bus
// but do not change the dispatch result.

namespace {

struct CapturedEvent {
  orangutan::hook::Event event;
  std::string tool_name;
  std::string input_json;
  std::string identity;
  bool succeeded{false};
  std::string output_text;
  std::optional<std::string> data_json{};
  orangutan::hook::ToolUsage usage{};
  std::string error_kind;
  std::string error_message;
  std::string verdict;
  std::string decision_reason;
  std::uint32_t replay_max{0};
  std::chrono::seconds approval_ttl{0};
};

class CaptureSink final : public orangutan::hook::Sink {
public:
  explicit CaptureSink(std::string id, orangutan::hook::SinkKind kind = orangutan::hook::SinkKind::default_)
      : id_(std::move(id)), kind_(kind) {}

  void set_blocking_decision(orangutan::hook::HookDecision decision) {
    blocking_decision_ = std::move(decision);
  }

  [[nodiscard]] std::string_view id() const noexcept override {
    return id_;
  }

  [[nodiscard]] orangutan::hook::SinkKind kind() const noexcept override {
    return kind_;
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> receive(orangutan::hook::Event event,
                                                             orangutan::hook::PayloadPtr payload) override {
    capture(event, *payload);
    co_return core::Result<void>{};
  }

  [[nodiscard]] async::Awaitable<core::Result<orangutan::hook::HookDecision>>
  handle_blocking(orangutan::hook::Event event, orangutan::hook::PayloadPtr payload) override {
    capture(event, *payload);
    co_return blocking_decision_;
  }

  [[nodiscard]] std::span<const CapturedEvent> captures() const noexcept {
    return captures_;
  }

private:
  void capture(orangutan::hook::Event event, const orangutan::hook::Payload& payload) {
    auto row = CapturedEvent{};
    row.event = event;
    std::visit(
        [&](auto& alt) {
          using T = std::decay_t<decltype(alt)>;
          if constexpr (std::same_as<T, orangutan::hook::ToolBeforePayload>) {
            row.tool_name = alt.tool_name;
            row.input_json = alt.input_json;
            row.identity = alt.who.identity;
          } else if constexpr (std::same_as<T, orangutan::hook::ToolDispatchedPayload>) {
            row.tool_name = alt.tool_name;
            row.input_json = alt.input_json;
            row.identity = alt.who.identity;
            row.verdict = alt.verdict;
          } else if constexpr (std::same_as<T, orangutan::hook::ToolAfterPayload>) {
            row.tool_name = alt.tool_name;
            row.input_json = alt.input_json;
            row.identity = alt.who.identity;
            row.succeeded = alt.succeeded;
            row.output_text = alt.output_text;
            row.data_json = alt.data_json;
            row.usage = alt.usage;
            row.error_kind = alt.error_kind;
            row.error_message = alt.error_message;
          } else if constexpr (std::same_as<T, orangutan::hook::ToolErrorPayload>) {
            row.tool_name = alt.tool_name;
            row.input_json = alt.input_json;
            row.identity = alt.who.identity;
            row.error_kind = alt.error_kind;
            row.error_message = alt.error_message;
          } else if constexpr (std::same_as<T, orangutan::hook::PermissionAskRenderedPayload>) {
            row.tool_name = alt.tool_name;
            row.input_json = alt.input_json;
            row.identity = alt.who.identity;
            row.decision_reason = alt.decision_reason;
            row.replay_max = alt.replay_max;
            row.approval_ttl = alt.approval_ttl;
          }
        },
        payload);
    captures_.push_back(std::move(row));
  }

  std::string id_;
  orangutan::hook::SinkKind kind_{orangutan::hook::SinkKind::default_};
  orangutan::hook::HookDecision blocking_decision_{};
  std::vector<CapturedEvent> captures_;
};

class FailingHookSink final : public orangutan::hook::Sink {
public:
  explicit FailingHookSink(std::string id) : id_(std::move(id)) {}

  [[nodiscard]] std::string_view id() const noexcept override {
    return id_;
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> receive(orangutan::hook::Event,
                                                             orangutan::hook::PayloadPtr) override {
    co_return std::unexpected(core::Error::internal("sink rejected").with("sink", id_));
  }

private:
  std::string id_;
};

class BlockingFailureSink final : public orangutan::hook::Sink {
public:
  explicit BlockingFailureSink(std::string id, bool throws = false) : id_(std::move(id)), throws_(throws) {}

  [[nodiscard]] std::string_view id() const noexcept override {
    return id_;
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> receive(orangutan::hook::Event,
                                                             orangutan::hook::PayloadPtr) override {
    co_return core::Result<void>{};
  }

  [[nodiscard]] async::Awaitable<core::Result<orangutan::hook::HookDecision>>
  handle_blocking(orangutan::hook::Event, orangutan::hook::PayloadPtr) override {
    if (throws_) {
      throw std::runtime_error{"blocking sink threw"};
    }
    co_return std::unexpected(core::Error::internal("blocking sink failed"));
  }

private:
  std::string id_;
  bool throws_{false};
};

class SlowBlockingHookSink final : public orangutan::hook::Sink {
public:
  SlowBlockingHookSink(std::string id, std::chrono::milliseconds delay) : id_(std::move(id)), delay_(delay) {}

  [[nodiscard]] std::string_view id() const noexcept override {
    return id_;
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> receive(orangutan::hook::Event,
                                                             orangutan::hook::PayloadPtr) override {
    co_return core::Result<void>{};
  }

  [[nodiscard]] async::Awaitable<core::Result<orangutan::hook::HookDecision>>
  handle_blocking(orangutan::hook::Event, orangutan::hook::PayloadPtr) override {
    ++calls_;
    const auto executor = co_await asio::this_coro::executor;
    auto slept = co_await async::sleep_for(executor, delay_);
    if (!slept) {
      co_return std::unexpected(std::move(slept).error());
    }
    co_return orangutan::hook::HookDecision{};
  }

  [[nodiscard]] std::size_t calls() const noexcept {
    return calls_;
  }

private:
  std::string id_;
  std::chrono::milliseconds delay_;
  std::size_t calls_{0};
};

permission::RuleSet allow_rule_set(std::string tool_pattern = "noop") {
  return single_rule(permission::Rule{
      .verdict = permission::Verdict::allow,
      .tool_pattern = std::move(tool_pattern),
  });
}

permission::RuleSet deny_rule_set(std::string tool_pattern = "noop") {
  return single_rule(permission::Rule{
      .verdict = permission::Verdict::deny,
      .tool_pattern = std::move(tool_pattern),
  });
}

async::Awaitable<core::Result<tool::Output>> noop_ok_handler(std::string_view, tool::DispatchContext& /*ctx*/) {
  co_return tool::Output::text_only("noop-ok");
}

async::Awaitable<core::Result<tool::Output>> noop_usage_handler(std::string_view, tool::DispatchContext& /*ctx*/) {
  co_return tool::Output{
      .text = "noop-usage",
      .usage =
          tool::ToolUsage{
              .bytes_read = 4096,
              .files_touched = 1,
              .match_count = 3,
              .wall_time = std::chrono::nanoseconds{42},
              .truncated = true,
          },
  };
}

async::Awaitable<core::Result<tool::Output>> noop_data_handler(std::string_view, tool::DispatchContext& /*ctx*/) {
  co_return tool::Output{
      .text = "noop-data",
      .data_json = std::string{R"({"kind":"noop","raw":true})"},
      .usage =
          tool::ToolUsage{
              .bytes_read = 7,
              .files_touched = 1,
          },
  };
}

async::Awaitable<core::Result<tool::Output>> noop_oversize_handler(std::string_view, tool::DispatchContext& /*ctx*/) {
  co_return tool::Output{
      .text = "abcdef",
      .data_json = std::string{"12345"},
  };
}

async::Awaitable<core::Result<tool::Output>> noop_error_handler(std::string_view, tool::DispatchContext& /*ctx*/) {
  co_return std::unexpected(core::Error::internal("handler exploded").with("tool", "noop"));
}

[[nodiscard]] core::ToolDef noop_tool_def() {
  return core::ToolDef{
      .name = "noop",
      .description = "noop",
      .input_schema_json = "{}",
      .required_capabilities = {},
      .deferred = false,
      .category = {},
  };
}

[[nodiscard]] core::ToolDef named_noop_tool_def(std::string_view name) {
  auto def = noop_tool_def();
  def.name = std::string{name};
  return def;
}

tool::DispatchContext make_hooked_ctx(asio::io_context& io,
                                      permission::RuleSet& rules,
                                      permission::AuditSink& sink,
                                      orangutan::hook::Bus* bus,
                                      permission::Mode mode = permission::Mode::default_) {
  return tool::DispatchContext{
      .executor = io.get_executor(),
      .mode = mode,
      .rules = rules,
      .audit = sink,
      .bus = bus,
      .scope_key = "scope-A",
      .agent_key = "coder",
      .identity = "operator-1",
  };
}

[[nodiscard]] std::string input_hash_hex(std::string_view input) {
  return permission::to_hex(permission::ApprovalAuthority::input_hash(input));
}

void require_redacted_file_write_input(const std::string& redacted_input,
                                       const std::string& original_input,
                                       std::string_view content) {
  const auto redacted = nlohmann::json::parse(redacted_input);
  REQUIRE(redacted["kind"] == "redacted_tool_input");
  REQUIRE(redacted["tool_name"] == std::string{tool::kFileWriteName});
  REQUIRE(redacted["input_hash"] == input_hash_hex(original_input));
  REQUIRE(redacted["input_bytes"] == original_input.size());
  REQUIRE(redacted["redacted_fields"] == nlohmann::json::array({"content"}));
  REQUIRE(redacted["content_bytes"] == content.size());
  REQUIRE(redacted["redaction_status"] == "ok");
}

void require_redacted_file_edit_input(const std::string& redacted_input,
                                      const std::string& original_input,
                                      std::string_view old_string,
                                      std::string_view new_string) {
  const auto redacted = nlohmann::json::parse(redacted_input);
  REQUIRE(redacted["kind"] == "redacted_tool_input");
  REQUIRE(redacted["tool_name"] == std::string{tool::kFileEditName});
  REQUIRE(redacted["input_hash"] == input_hash_hex(original_input));
  REQUIRE(redacted["input_bytes"] == original_input.size());
  REQUIRE(redacted["redacted_fields"] == nlohmann::json::array({"old_string", "new_string"}));
  REQUIRE(redacted["old_string_bytes"] == old_string.size());
  REQUIRE(redacted["new_string_bytes"] == new_string.size());
  REQUIRE(redacted["redaction_status"] == "ok");
}

void require_redacted_memory_remember_input(const std::string& redacted_input, const std::string& original_input) {
  const auto redacted = nlohmann::json::parse(redacted_input);
  REQUIRE(redacted == nlohmann::json{
                          {"kind", "redacted_tool_input"},
                          {"tool_name", std::string{tool::kMemoryRememberName}},
                          {"input_hash", input_hash_hex(original_input)},
                          {"input_bytes", original_input.size()},
                          {"redaction_status", "ok"},
                      });
}

}  // namespace

TEST_CASE("dispatch publishes tool_before + tool_after on the allow path", "[unit][tool][hook]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(noop_tool_def(), &noop_ok_handler).has_value());

    auto rules = allow_rule_set();
    permission::RecordingAuditSink audit;

    orangutan::hook::Bus bus;
    CaptureSink sink{"capture-1"};
    bus.bind(sink, {orangutan::hook::Event::tool_before, orangutan::hook::Event::tool_after});

    auto ctx = make_hooked_ctx(io, rules, audit, &bus);
    auto result = co_await registry.dispatch("noop", R"({"k":1})", ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text == "noop-ok");

    REQUIRE(sink.captures().size() == 2);
    REQUIRE(sink.captures()[0].event == orangutan::hook::Event::tool_before);
    REQUIRE(sink.captures()[0].tool_name == "noop");
    REQUIRE(sink.captures()[0].identity == "operator-1");
    REQUIRE(sink.captures()[1].event == orangutan::hook::Event::tool_after);
    REQUIRE(sink.captures()[1].tool_name == "noop");
    REQUIRE(sink.captures()[1].succeeded);
    REQUIRE(sink.captures()[1].output_text == "noop-ok");
    REQUIRE(sink.captures()[1].error_kind.empty());

    REQUIRE(audit.events().size() == 1);
    auto metadata = nlohmann::json::parse(audit.events()[0].metadata_json);
    REQUIRE(metadata.contains("hook_decisions"));
    REQUIRE(metadata["hook_decisions"].size() == 1);
    REQUIRE(metadata["hook_decisions"][0]["sink_id"] == "capture-1");
    REQUIRE(metadata["hook_decisions"][0]["kind"] == "proceed");
    REQUIRE(metadata["hook_decisions"][0]["reason"] == "");
  });
}

TEST_CASE("blocking tool_before veto skips the handler and records blocked_by_hook", "[unit][tool][hook][blocking]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    std::size_t handler_calls = 0;
    tool::Registry registry;
    REQUIRE(registry
                .add(noop_tool_def(),
                     [&](std::string_view /*input*/,
                         tool::DispatchContext& /*ctx*/) -> async::Awaitable<core::Result<tool::Output>> {
                       ++handler_calls;
                       co_return tool::Output::text_only("should-not-run");
                     })
                .has_value());

    auto rules = allow_rule_set();
    permission::RecordingAuditSink audit;

    orangutan::hook::HookDecision veto{};
    veto.kind = orangutan::hook::HookDecisionKind::veto;
    veto.reason = "policy";

    orangutan::hook::Bus bus;
    CaptureSink first{"first"};
    CaptureSink blocker{"blocker"};
    blocker.set_blocking_decision(veto);
    CaptureSink late{"late"};
    bus.bind(first, {orangutan::hook::Event::tool_before});
    bus.bind(blocker, {orangutan::hook::Event::tool_before, orangutan::hook::Event::tool_after});
    bus.bind(late, {orangutan::hook::Event::tool_before});

    const std::string input = R"({"k":1})";
    auto ctx = make_hooked_ctx(io, rules, audit, &bus);
    auto result = co_await registry.dispatch("noop", input, ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(context_has(result.error(), "reason", "blocked_by_hook"));
    REQUIRE(context_has(result.error(), "hook_reason", "policy"));
    REQUIRE(handler_calls == 0);

    REQUIRE(first.captures().size() == 1);
    REQUIRE(first.captures()[0].event == orangutan::hook::Event::tool_before);
    REQUIRE(blocker.captures().size() == 2);
    REQUIRE(blocker.captures()[0].event == orangutan::hook::Event::tool_before);
    REQUIRE(blocker.captures()[1].event == orangutan::hook::Event::tool_after);
    REQUIRE_FALSE(blocker.captures()[1].succeeded);
    REQUIRE(blocker.captures()[1].error_kind == "blocked_by_hook");
    REQUIRE(late.captures().empty());

    REQUIRE(audit.events().size() == 1);
    const auto& event = audit.events()[0];
    REQUIRE(event.outcome == permission::AuditOutcome::blocked_by_hook);
    REQUIRE(event.reason == "policy");
    REQUIRE(event.input_hash.has_value());
    REQUIRE(permission::to_hex(*event.input_hash) == input_hash_hex(input));

    auto metadata = nlohmann::json::parse(event.metadata_json);
    REQUIRE(metadata["original_input_hash"] == input_hash_hex(input));
    REQUIRE_FALSE(metadata.contains("rewritten_input_hash"));
    REQUIRE(metadata["hook_decisions"].size() == 2);
    REQUIRE(metadata["hook_decisions"][0]["sink_id"] == "first");
    REQUIRE(metadata["hook_decisions"][0]["kind"] == "proceed");
    REQUIRE(metadata["hook_decisions"][1]["sink_id"] == "blocker");
    REQUIRE(metadata["hook_decisions"][1]["kind"] == "veto");
    REQUIRE(metadata["hook_decisions"][1]["reason"] == "policy");
  });
}

TEST_CASE("blocking tool_before writes a joinable hook_publish row for traced dispatch",
          "[unit][tool][hook][blocking][trace]") {
  TempDb db{"oran-tool-hook-publish"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    std::size_t handler_calls = 0;
    tool::Registry registry;
    REQUIRE(registry
                .add(noop_tool_def(),
                     [&](std::string_view /*input*/,
                         tool::DispatchContext& /*ctx*/) -> async::Awaitable<core::Result<tool::Output>> {
                       ++handler_calls;
                       co_return tool::Output::text_only("should-not-run");
                     })
                .has_value());

    auto pool_result = storage::Pool::open(
        io.get_executor(),
        storage::PoolOptions{.path = db.string(), .reader_count = 2, .statement_cache_capacity = 8});
    REQUIRE(pool_result.has_value());
    auto pool = std::move(*pool_result);
    storage::AuditRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());
    permission::StorageAuditSink audit{repo, io.get_executor()};

    auto rules = allow_rule_set();
    orangutan::hook::HookDecision veto{};
    veto.kind = orangutan::hook::HookDecisionKind::veto;
    veto.reason = "policy";

    orangutan::hook::Bus bus;
    CaptureSink first{"first"};
    CaptureSink blocker{"blocker"};
    blocker.set_blocking_decision(veto);
    bus.bind(first, {orangutan::hook::Event::tool_before});
    bus.bind(blocker, {orangutan::hook::Event::tool_before});

    auto ctx = make_hooked_ctx(io, rules, audit, &bus);
    ctx.parent_turn_id = turn_id_with(0x72);
    auto result = co_await registry.dispatch("noop", R"({"k":1})", ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(handler_calls == 0);

    auto joined = co_await repo.list_events_for_turn(*ctx.parent_turn_id);
    REQUIRE(joined.has_value());
    REQUIRE(joined->size() == 2);
    REQUIRE((*joined)[0].event_kind == "hook_publish");
    REQUIRE((*joined)[0].parent_turn_id == ctx.parent_turn_id);
    REQUIRE((*joined)[0].reason == "policy");
    auto hook_metadata = nlohmann::json::parse((*joined)[0].metadata_json);
    REQUIRE(hook_metadata["event"] == "tool_before");
    REQUIRE(hook_metadata["sink_id"] == "blocker");
    REQUIRE(hook_metadata["decision_kind"] == "veto");
    REQUIRE(hook_metadata["reason"] == "policy");
    REQUIRE(hook_metadata["hook_decisions"].size() == 2);
    REQUIRE(hook_metadata["hook_decisions"][0]["sink_id"] == "first");
    REQUIRE(hook_metadata["hook_decisions"][0]["kind"] == "proceed");
    REQUIRE(hook_metadata["hook_decisions"][1]["sink_id"] == "blocker");
    REQUIRE(hook_metadata["hook_decisions"][1]["kind"] == "veto");

    REQUIRE((*joined)[1].event_kind == "permission_decision");
    REQUIRE((*joined)[1].outcome == "blocked_by_hook");
    REQUIRE((*joined)[1].reason == "policy");
  });
}

TEST_CASE("blocking tool_before rewrite feeds permission, handler, audit, and hooks the rewritten input",
          "[unit][tool][hook][blocking]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(noop_tool_def(), make_echo_handler()).has_value());

    auto denied_original = permission::InputPattern::compile("danger");
    REQUIRE(denied_original.has_value());
    permission::RuleSet rules;
    rules.push_back(permission::Rule{
        .verdict = permission::Verdict::deny,
        .tool_pattern = "noop",
        .input_pattern = std::move(*denied_original),
    });
    rules.push_back(permission::Rule{.verdict = permission::Verdict::allow, .tool_pattern = "noop"});
    permission::RecordingAuditSink audit;

    const std::string original_input = R"({"mode":"danger"})";
    const std::string rewritten_input = R"({"mode":"safe"})";
    orangutan::hook::HookDecision rewrite{};
    rewrite.kind = orangutan::hook::HookDecisionKind::rewrite;
    rewrite.reason = "redacted";
    rewrite.rewritten_input_json = rewritten_input;

    orangutan::hook::Bus bus;
    CaptureSink sink{"rewriter"};
    sink.set_blocking_decision(rewrite);
    bus.bind(sink,
             {orangutan::hook::Event::tool_before,
              orangutan::hook::Event::tool_dispatched,
              orangutan::hook::Event::tool_after});

    auto ctx = make_hooked_ctx(io, rules, audit, &bus);
    auto result = co_await registry.dispatch("noop", original_input, ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text == rewritten_input);

    REQUIRE(sink.captures().size() == 3);
    REQUIRE(sink.captures()[0].event == orangutan::hook::Event::tool_before);
    REQUIRE(sink.captures()[0].input_json == original_input);
    REQUIRE(sink.captures()[1].event == orangutan::hook::Event::tool_dispatched);
    REQUIRE(sink.captures()[1].input_json == rewritten_input);
    REQUIRE(sink.captures()[2].event == orangutan::hook::Event::tool_after);
    REQUIRE(sink.captures()[2].input_json == rewritten_input);
    REQUIRE(sink.captures()[2].succeeded);

    REQUIRE(audit.events().size() == 1);
    const auto& event = audit.events()[0];
    REQUIRE(event.verdict == permission::Verdict::allow);
    REQUIRE(event.outcome == permission::AuditOutcome::rewritten);
    REQUIRE(event.input_hash.has_value());
    REQUIRE(permission::to_hex(*event.input_hash) == input_hash_hex(rewritten_input));

    auto metadata = nlohmann::json::parse(event.metadata_json);
    REQUIRE(metadata["original_input_hash"] == input_hash_hex(original_input));
    REQUIRE(metadata["rewritten_input_hash"] == input_hash_hex(rewritten_input));
    REQUIRE(metadata["hook_decisions"].size() == 1);
    REQUIRE(metadata["hook_decisions"][0]["sink_id"] == "rewriter");
    REQUIRE(metadata["hook_decisions"][0]["kind"] == "rewrite");
    REQUIRE(metadata["hook_decisions"][0]["reason"] == "redacted");
  });
}

TEST_CASE("blocking tool_before require_approval promotes allow through the broker",
          "[unit][tool][hook][blocking][approval]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(core::ToolDef::with_no_input("noop", "noop"), make_echo_handler()).has_value());

    auto rules = allow_rule_set();
    permission::RecordingAuditSink audit;
    auto broker = make_broker();
    const auto now = fixed_now();
    const std::string input = R"({"requires":"operator"})";
    const auto token = grant(broker, "noop", input, "operator-1", now);

    orangutan::hook::HookDecision require_approval{};
    require_approval.kind = orangutan::hook::HookDecisionKind::require_approval;
    require_approval.reason = "operator_review";

    orangutan::hook::Bus bus;
    CaptureSink sink{"approval-hook"};
    sink.set_blocking_decision(require_approval);
    bus.bind(sink, {orangutan::hook::Event::tool_before, orangutan::hook::Event::tool_after});

    auto ctx = make_approval_ctx(io, rules, audit, &broker, &token, now);
    ctx.bus = &bus;

    auto result = co_await registry.dispatch("noop", input, ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text == input);

    REQUIRE(sink.captures().size() == 2);
    REQUIRE(sink.captures()[0].event == orangutan::hook::Event::tool_before);
    REQUIRE(sink.captures()[1].event == orangutan::hook::Event::tool_after);
    REQUIRE(sink.captures()[1].succeeded);

    REQUIRE(audit.events().size() == 1);
    const auto& event = audit.events()[0];
    REQUIRE(event.verdict == permission::Verdict::ask);
    REQUIRE(event.outcome == permission::AuditOutcome::approved);
    REQUIRE(event.reason == "operator_review");
    auto metadata = nlohmann::json::parse(event.metadata_json);
    REQUIRE(metadata["hook_decisions"].size() == 1);
    REQUIRE(metadata["hook_decisions"][0]["sink_id"] == "approval-hook");
    REQUIRE(metadata["hook_decisions"][0]["kind"] == "require_approval");
    REQUIRE(metadata["hook_decisions"][0]["reason"] == "operator_review");
  });
}

TEST_CASE("blocking tool_before require_approval preserves a permission deny",
          "[unit][tool][hook][blocking][approval]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(core::ToolDef::with_no_input("noop", "noop"), make_echo_handler()).has_value());

    auto rules = deny_rule_set();
    permission::RecordingAuditSink audit;

    orangutan::hook::HookDecision require_approval{};
    require_approval.kind = orangutan::hook::HookDecisionKind::require_approval;
    require_approval.reason = "operator_review";

    orangutan::hook::Bus bus;
    CaptureSink sink{"approval-hook"};
    sink.set_blocking_decision(require_approval);
    bus.bind(sink, {orangutan::hook::Event::tool_before, orangutan::hook::Event::tool_after});

    auto ctx = make_hooked_ctx(io, rules, audit, &bus);

    auto result = co_await registry.dispatch("noop", R"({"requires":"operator"})", ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(context_has(result.error(), "reason", "rule #0 (deny: noop)"));

    REQUIRE(sink.captures().size() == 2);
    REQUIRE(sink.captures()[0].event == orangutan::hook::Event::tool_before);
    REQUIRE(sink.captures()[1].event == orangutan::hook::Event::tool_after);
    REQUIRE_FALSE(sink.captures()[1].succeeded);
    REQUIRE(sink.captures()[1].error_kind == "permission_denied");

    REQUIRE(audit.events().size() == 1);
    const auto& event = audit.events()[0];
    REQUIRE(event.verdict == permission::Verdict::deny);
    REQUIRE(event.outcome == permission::AuditOutcome::deny);
    REQUIRE(event.reason == "rule #0 (deny: noop)");
    auto metadata = nlohmann::json::parse(event.metadata_json);
    REQUIRE(metadata["hook_decisions"].size() == 1);
    REQUIRE(metadata["hook_decisions"][0]["sink_id"] == "approval-hook");
    REQUIRE(metadata["hook_decisions"][0]["kind"] == "require_approval");
  });
}

TEST_CASE("blocking tool_before rewrite without replacement is recorded as blocked_by_hook",
          "[unit][tool][hook][blocking]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    std::size_t handler_calls = 0;
    tool::Registry registry;
    REQUIRE(registry
                .add(noop_tool_def(),
                     [&](std::string_view /*input*/,
                         tool::DispatchContext& /*ctx*/) -> async::Awaitable<core::Result<tool::Output>> {
                       ++handler_calls;
                       co_return tool::Output::text_only("should-not-run");
                     })
                .has_value());

    auto rules = allow_rule_set();
    permission::RecordingAuditSink audit;

    orangutan::hook::HookDecision rewrite{};
    rewrite.kind = orangutan::hook::HookDecisionKind::rewrite;
    rewrite.reason = "redact";

    orangutan::hook::Bus bus;
    CaptureSink sink{"rewriter"};
    sink.set_blocking_decision(rewrite);
    bus.bind(sink, {orangutan::hook::Event::tool_before, orangutan::hook::Event::tool_after});

    const std::string input = R"({"mode":"danger"})";
    auto ctx = make_hooked_ctx(io, rules, audit, &bus);

    auto result = co_await registry.dispatch("noop", input, ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(context_has(result.error(), "reason", "blocked_by_hook"));
    REQUIRE(context_has(result.error(), "hook_reason", "hook rewrite missing rewritten_input_json"));
    REQUIRE(handler_calls == 0);

    REQUIRE(sink.captures().size() == 2);
    REQUIRE(sink.captures()[0].event == orangutan::hook::Event::tool_before);
    REQUIRE(sink.captures()[1].event == orangutan::hook::Event::tool_after);
    REQUIRE_FALSE(sink.captures()[1].succeeded);
    REQUIRE(sink.captures()[1].error_kind == "blocked_by_hook");

    REQUIRE(audit.events().size() == 1);
    const auto& event = audit.events()[0];
    REQUIRE(event.outcome == permission::AuditOutcome::blocked_by_hook);
    REQUIRE(event.reason == "hook rewrite missing rewritten_input_json");
    REQUIRE(event.input_hash.has_value());
    REQUIRE(permission::to_hex(*event.input_hash) == input_hash_hex(input));

    auto metadata = nlohmann::json::parse(event.metadata_json);
    REQUIRE(metadata["original_input_hash"] == input_hash_hex(input));
    REQUIRE_FALSE(metadata.contains("rewritten_input_hash"));
    REQUIRE(metadata["hook_decisions"].size() == 1);
    REQUIRE(metadata["hook_decisions"][0]["sink_id"] == "rewriter");
    REQUIRE(metadata["hook_decisions"][0]["kind"] == "rewrite");
    REQUIRE(metadata["hook_decisions"][0]["reason"] == "redact");
  });
}

TEST_CASE("blocking tool_before sink error is recorded as blocked_by_hook", "[unit][tool][hook][blocking]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    std::size_t handler_calls = 0;
    tool::Registry registry;
    REQUIRE(registry
                .add(noop_tool_def(),
                     [&](std::string_view /*input*/,
                         tool::DispatchContext& /*ctx*/) -> async::Awaitable<core::Result<tool::Output>> {
                       ++handler_calls;
                       co_return tool::Output::text_only("should-not-run");
                     })
                .has_value());

    auto rules = allow_rule_set();
    permission::RecordingAuditSink audit;

    orangutan::hook::Bus bus;
    CaptureSink first{"first"};
    BlockingFailureSink failing{"failing"};
    CaptureSink late{"late"};
    bus.bind(first, {orangutan::hook::Event::tool_before});
    bus.bind(failing, {orangutan::hook::Event::tool_before, orangutan::hook::Event::tool_after});
    bus.bind(late, {orangutan::hook::Event::tool_before});

    auto ctx = make_hooked_ctx(io, rules, audit, &bus);
    ctx.parent_turn_id = turn_id_with(0x73);
    auto result = co_await registry.dispatch("noop", R"({})", ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(context_has(result.error(), "reason", "blocked_by_hook"));
    REQUIRE(handler_calls == 0);

    REQUIRE(first.captures().size() == 1);
    REQUIRE(late.captures().empty());

    REQUIRE(audit.events().size() == 2);
    REQUIRE(audit.events()[0].event_kind == "hook_publish");
    REQUIRE(audit.events()[0].parent_turn_id == ctx.parent_turn_id);
    auto hook_publish_metadata = nlohmann::json::parse(audit.events()[0].metadata_json);
    REQUIRE(hook_publish_metadata["event"] == "tool_before");
    REQUIRE(hook_publish_metadata["sink_id"] == "failing");
    REQUIRE(hook_publish_metadata["decision_kind"] == "veto");
    REQUIRE(hook_publish_metadata["error"].get<std::string>().contains("blocking sink failed"));

    const auto& event = audit.events()[1];
    REQUIRE(event.outcome == permission::AuditOutcome::blocked_by_hook);
    REQUIRE(event.reason.starts_with("hook_error"));
    REQUIRE(event.reason.contains("blocking sink failed"));

    auto metadata = nlohmann::json::parse(event.metadata_json);
    REQUIRE(metadata["hook_decisions"].size() == 2);
    REQUIRE(metadata["hook_decisions"][0]["kind"] == "proceed");
    REQUIRE(metadata["hook_decisions"][1]["sink_id"] == "failing");
    REQUIRE(metadata["hook_decisions"][1]["kind"] == "veto");
    REQUIRE(metadata["hook_decisions"][1]["reason"].get<std::string>().contains("blocking sink failed"));
  });
}

TEST_CASE("blocking tool_before timeout is recorded as blocked_by_hook", "[unit][tool][hook][blocking]") {
  test::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        std::size_t handler_calls = 0;
        tool::Registry registry;
        REQUIRE(registry
                    .add(noop_tool_def(),
                         [&](std::string_view /*input*/,
                             tool::DispatchContext& /*ctx*/) -> async::Awaitable<core::Result<tool::Output>> {
                           ++handler_calls;
                           co_return tool::Output::text_only("should-not-run");
                         })
                    .has_value());

        auto rules = allow_rule_set();
        permission::RecordingAuditSink audit;

        orangutan::hook::Bus bus{orangutan::hook::BusOptions{.blocking_timeout = std::chrono::milliseconds{5}}};
        SlowBlockingHookSink slow{"slow", std::chrono::seconds{1}};
        CaptureSink late{"late"};
        bus.bind(slow, {orangutan::hook::Event::tool_before});
        bus.bind(late, {orangutan::hook::Event::tool_before});

        auto ctx = make_hooked_ctx(io, rules, audit, &bus);
        auto result = co_await registry.dispatch("noop", R"({"k":1})", ctx);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().kind() == core::ErrorKind::permission_denied);
        REQUIRE(context_has(result.error(), "reason", "blocked_by_hook"));
        REQUIRE(context_has(result.error(), "hook_reason", "hook_timeout"));
        REQUIRE(handler_calls == 0);
        REQUIRE(slow.calls() == 1);
        REQUIRE(late.captures().empty());

        REQUIRE(audit.events().size() == 1);
        const auto& event = audit.events()[0];
        REQUIRE(event.outcome == permission::AuditOutcome::blocked_by_hook);
        REQUIRE(event.reason == "hook_timeout");

        auto metadata = nlohmann::json::parse(event.metadata_json);
        REQUIRE(metadata["hook_decisions"].size() == 1);
        REQUIRE(metadata["hook_decisions"][0]["sink_id"] == "slow");
        REQUIRE(metadata["hook_decisions"][0]["kind"] == "veto");
        REQUIRE(metadata["hook_decisions"][0]["reason"] == "hook_timeout");
        REQUIRE(metadata["hook_decisions"][0]["elapsed_ms"] == 5);
        co_return;
      },
      std::chrono::milliseconds{250});
}

TEST_CASE("dispatch copies output usage into tool_after payload", "[unit][tool][hook][output]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(noop_tool_def(), &noop_usage_handler).has_value());

    auto rules = allow_rule_set();
    permission::RecordingAuditSink audit;

    orangutan::hook::Bus bus;
    CaptureSink sink{"capture-usage"};
    bus.bind(sink, {orangutan::hook::Event::tool_after});

    auto ctx = make_hooked_ctx(io, rules, audit, &bus);
    auto result = co_await registry.dispatch("noop", R"({})", ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text == "noop-usage");

    REQUIRE(sink.captures().size() == 1);
    REQUIRE(sink.captures()[0].event == orangutan::hook::Event::tool_after);
    REQUIRE(sink.captures()[0].succeeded);
    REQUIRE(sink.captures()[0].output_text == "noop-usage");
    REQUIRE(sink.captures()[0].usage.bytes_read == 4096);
    REQUIRE(sink.captures()[0].usage.files_touched == 1);
    REQUIRE(sink.captures()[0].usage.match_count == 3);
    REQUIRE(sink.captures()[0].usage.wall_time == std::chrono::nanoseconds{42});
    REQUIRE(sink.captures()[0].usage.truncated);
    REQUIRE_FALSE(sink.captures()[0].usage.data_dropped);

    REQUIRE(audit.events().size() == 1);
    auto metadata = nlohmann::json::parse(audit.events()[0].metadata_json);
    REQUIRE(metadata.contains("usage"));
    REQUIRE(metadata["usage"]["bytes_read"] == 4096);
    REQUIRE(metadata["usage"]["files_touched"] == 1);
    REQUIRE(metadata["usage"]["match_count"] == 3);
    REQUIRE(metadata["usage"]["truncated"] == true);
    const auto wall_time_ms = metadata["usage"]["wall_time_ms"].get<double>();
    REQUIRE(wall_time_ms > 0.0);
    REQUIRE(wall_time_ms < 0.001);
  });
}

TEST_CASE("dispatch redacts structured output from untrusted tool_after sinks", "[unit][tool][hook][output]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(noop_tool_def(), &noop_data_handler).has_value());

    auto rules = allow_rule_set();
    permission::RecordingAuditSink audit;

    orangutan::hook::Bus bus;
    CaptureSink default_sink{"capture-default"};
    CaptureSink trusted_sink{"capture-trusted", orangutan::hook::SinkKind::trusted_local};
    bus.bind(default_sink, {orangutan::hook::Event::tool_after});
    bus.bind(trusted_sink, {orangutan::hook::Event::tool_after});

    auto ctx = make_hooked_ctx(io, rules, audit, &bus);
    auto result = co_await registry.dispatch("noop", R"({})", ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text == "noop-data");
    REQUIRE(result->data_json == R"({"kind":"noop","raw":true})");

    REQUIRE(default_sink.captures().size() == 1);
    REQUIRE(default_sink.captures()[0].succeeded);
    REQUIRE(default_sink.captures()[0].output_text == "noop-data");
    REQUIRE_FALSE(default_sink.captures()[0].data_json.has_value());
    REQUIRE(default_sink.captures()[0].usage.bytes_read == 7);

    REQUIRE(trusted_sink.captures().size() == 1);
    REQUIRE(trusted_sink.captures()[0].succeeded);
    REQUIRE(trusted_sink.captures()[0].output_text == "noop-data");
    REQUIRE(trusted_sink.captures()[0].data_json == R"({"kind":"noop","raw":true})");
    REQUIRE(trusted_sink.captures()[0].usage.files_touched == 1);
  });
}

TEST_CASE("dispatch redacts FileWrite input for non-trusted hook sinks", "[unit][tool][hook][redaction]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(named_noop_tool_def(tool::kFileWriteName), &noop_ok_handler).has_value());

    auto rules = allow_rule_set(std::string{tool::kFileWriteName});
    permission::RecordingAuditSink audit;

    orangutan::hook::Bus bus;
    CaptureSink default_sink{"capture-default"};
    CaptureSink trusted_sink{"capture-trusted", orangutan::hook::SinkKind::trusted_local};
    bus.bind(default_sink,
             {orangutan::hook::Event::tool_before,
              orangutan::hook::Event::tool_dispatched,
              orangutan::hook::Event::tool_after});
    bus.bind(trusted_sink,
             {orangutan::hook::Event::tool_before,
              orangutan::hook::Event::tool_dispatched,
              orangutan::hook::Event::tool_after});

    const std::string input = R"({"path":"notes.md","content":"top-secret"})";
    auto ctx = make_hooked_ctx(io, rules, audit, &bus);
    auto result = co_await registry.dispatch(tool::kFileWriteName, input, ctx);
    REQUIRE(result.has_value());

    REQUIRE(default_sink.captures().size() == 3);
    for (const auto& capture : default_sink.captures()) {
      REQUIRE_FALSE(capture.input_json.contains("notes.md"));
      REQUIRE_FALSE(capture.input_json.contains("top-secret"));
      require_redacted_file_write_input(capture.input_json, input, "top-secret");
    }

    REQUIRE(trusted_sink.captures().size() == 3);
    for (const auto& capture : trusted_sink.captures()) {
      REQUIRE(capture.input_json == input);
    }
  });
}

TEST_CASE("dispatch redacts FileEdit input for non-trusted hook sinks", "[unit][tool][hook][redaction]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(named_noop_tool_def(tool::kFileEditName), &noop_ok_handler).has_value());

    auto rules = allow_rule_set(std::string{tool::kFileEditName});
    permission::RecordingAuditSink audit;

    orangutan::hook::Bus bus;
    CaptureSink default_sink{"capture-default"};
    CaptureSink trusted_sink{"capture-trusted", orangutan::hook::SinkKind::trusted_local};
    bus.bind(default_sink,
             {orangutan::hook::Event::tool_before,
              orangutan::hook::Event::tool_dispatched,
              orangutan::hook::Event::tool_after});
    bus.bind(trusted_sink,
             {orangutan::hook::Event::tool_before,
              orangutan::hook::Event::tool_dispatched,
              orangutan::hook::Event::tool_after});

    const std::string input =
        R"({"path":"notes.md","old_string":"old-secret","new_string":"new-secret","replace_all":true})";
    auto ctx = make_hooked_ctx(io, rules, audit, &bus);
    auto result = co_await registry.dispatch(tool::kFileEditName, input, ctx);
    REQUIRE(result.has_value());

    REQUIRE(default_sink.captures().size() == 3);
    for (const auto& capture : default_sink.captures()) {
      REQUIRE_FALSE(capture.input_json.contains("notes.md"));
      REQUIRE_FALSE(capture.input_json.contains("old-secret"));
      REQUIRE_FALSE(capture.input_json.contains("new-secret"));
      require_redacted_file_edit_input(capture.input_json, input, "old-secret", "new-secret");
    }

    REQUIRE(trusted_sink.captures().size() == 3);
    for (const auto& capture : trusted_sink.captures()) {
      REQUIRE(capture.input_json == input);
    }
  });
}

TEST_CASE("dispatch redacts MemoryRemember input for non-trusted hook sinks",
          "[unit][tool][hook][redaction][memory_remember]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(named_noop_tool_def(tool::kMemoryRememberName), &noop_ok_handler).has_value());

    auto rules = allow_rule_set(std::string{tool::kMemoryRememberName});
    permission::RecordingAuditSink audit;

    orangutan::hook::Bus bus;
    CaptureSink default_sink{"capture-default"};
    CaptureSink trusted_sink{"capture-trusted", orangutan::hook::SinkKind::trusted_local};
    bus.bind(default_sink,
             {orangutan::hook::Event::tool_before,
              orangutan::hook::Event::tool_dispatched,
              orangutan::hook::Event::tool_after});
    bus.bind(trusted_sink,
             {orangutan::hook::Event::tool_before,
              orangutan::hook::Event::tool_dispatched,
              orangutan::hook::Event::tool_after});

    const std::string input =
        R"({"id":"private-id","kind":"fact","title":"Sensitive title","body":"Sensitive body","tags":["private-tag"],"linked_record_ids":["linked-private-id"]})";
    auto ctx = make_hooked_ctx(io, rules, audit, &bus);
    auto result = co_await registry.dispatch(tool::kMemoryRememberName, input, ctx);
    REQUIRE(result.has_value());

    REQUIRE(default_sink.captures().size() == 3);
    for (const auto& capture : default_sink.captures()) {
      REQUIRE_FALSE(capture.input_json.contains("private-id"));
      REQUIRE_FALSE(capture.input_json.contains("Sensitive title"));
      REQUIRE_FALSE(capture.input_json.contains("Sensitive body"));
      REQUIRE_FALSE(capture.input_json.contains("private-tag"));
      REQUIRE_FALSE(capture.input_json.contains("linked-private-id"));
      require_redacted_memory_remember_input(capture.input_json, input);
    }

    REQUIRE(trusted_sink.captures().size() == 3);
    for (const auto& capture : trusted_sink.captures()) {
      REQUIRE(capture.input_json == input);
    }
  });
}

TEST_CASE("dispatch redacts FileWrite input on tool_error for non-trusted hook sinks",
          "[unit][tool][hook][redaction]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(named_noop_tool_def(tool::kFileWriteName), &noop_error_handler).has_value());

    auto rules = allow_rule_set(std::string{tool::kFileWriteName});
    permission::RecordingAuditSink audit;

    orangutan::hook::Bus bus;
    CaptureSink default_sink{"capture-default"};
    CaptureSink trusted_sink{"capture-trusted", orangutan::hook::SinkKind::trusted_local};
    bus.bind(default_sink, {orangutan::hook::Event::tool_error});
    bus.bind(trusted_sink, {orangutan::hook::Event::tool_error});

    const std::string input = R"({"path":"notes.md","content":"top-secret"})";
    auto ctx = make_hooked_ctx(io, rules, audit, &bus);
    auto result = co_await registry.dispatch(tool::kFileWriteName, input, ctx);
    REQUIRE_FALSE(result.has_value());

    REQUIRE(default_sink.captures().size() == 1);
    REQUIRE(default_sink.captures()[0].event == orangutan::hook::Event::tool_error);
    REQUIRE(default_sink.captures()[0].error_kind == "internal");
    REQUIRE_FALSE(default_sink.captures()[0].input_json.contains("notes.md"));
    REQUIRE_FALSE(default_sink.captures()[0].input_json.contains("top-secret"));
    require_redacted_file_write_input(default_sink.captures()[0].input_json, input, "top-secret");

    REQUIRE(trusted_sink.captures().size() == 1);
    REQUIRE(trusted_sink.captures()[0].input_json == input);
  });
}

TEST_CASE("dispatch redacts FileWrite input in permission ask payloads for non-trusted hook sinks",
          "[unit][tool][hook][redaction][approval]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(named_noop_tool_def(tool::kFileWriteName), make_echo_handler()).has_value());
    auto rules = single_rule(permission::Rule{
        .verdict = permission::Verdict::ask,
        .tool_pattern = std::string{tool::kFileWriteName},
    });
    permission::RecordingAuditSink audit;
    auto broker = make_broker();

    orangutan::hook::HookDecision approved{};
    approved.reason = "operator_approved:operator-1";

    orangutan::hook::Bus bus;
    CaptureSink default_prompt{"default-prompt"};
    default_prompt.set_blocking_decision(approved);
    CaptureSink trusted_prompt{"trusted-prompt", orangutan::hook::SinkKind::trusted_local};
    trusted_prompt.set_blocking_decision(approved);
    bus.bind(default_prompt, {orangutan::hook::Event::permission_ask_rendered});
    bus.bind(trusted_prompt, {orangutan::hook::Event::permission_ask_rendered});

    const std::string input = R"({"path":"notes.md","content":"top-secret"})";
    auto ctx = make_approval_ctx(io, rules, audit, &broker, /*token=*/nullptr, fixed_now());
    ctx.bus = &bus;

    auto result = co_await registry.dispatch(tool::kFileWriteName, input, ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text == input);

    REQUIRE(default_prompt.captures().size() == 1);
    REQUIRE(default_prompt.captures()[0].event == orangutan::hook::Event::permission_ask_rendered);
    REQUIRE_FALSE(default_prompt.captures()[0].input_json.contains("notes.md"));
    REQUIRE_FALSE(default_prompt.captures()[0].input_json.contains("top-secret"));
    require_redacted_file_write_input(default_prompt.captures()[0].input_json, input, "top-secret");

    REQUIRE(trusted_prompt.captures().size() == 1);
    REQUIRE(trusted_prompt.captures()[0].event == orangutan::hook::Event::permission_ask_rendered);
    REQUIRE(trusted_prompt.captures()[0].input_json == input);
  });
}

TEST_CASE("dispatch applies output caps before returning and publishing tool_after",
          "[unit][tool][hook][output][caps]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(noop_tool_def(), &noop_oversize_handler).has_value());

    auto rules = allow_rule_set();
    permission::RecordingAuditSink audit;

    orangutan::hook::Bus bus;
    CaptureSink trusted_sink{"capture-cap", orangutan::hook::SinkKind::trusted_local};
    bus.bind(trusted_sink, {orangutan::hook::Event::tool_after});

    auto ctx = make_hooked_ctx(io, rules, audit, &bus);
    ctx.output_caps = tool::OutputCapOptions{.max_text_bytes = 4, .max_data_bytes = 4};

    auto result = co_await registry.dispatch("noop", R"({})", ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text == "abcd");
    REQUIRE_FALSE(result->data_json.has_value());
    REQUIRE(result->usage.truncated);
    REQUIRE(result->usage.data_dropped);

    REQUIRE(trusted_sink.captures().size() == 1);
    REQUIRE(trusted_sink.captures()[0].succeeded);
    REQUIRE(trusted_sink.captures()[0].output_text == "abcd");
    REQUIRE_FALSE(trusted_sink.captures()[0].data_json.has_value());
    REQUIRE(trusted_sink.captures()[0].usage.truncated);
    REQUIRE(trusted_sink.captures()[0].usage.data_dropped);

    REQUIRE(audit.events().size() == 1);
    auto metadata = nlohmann::json::parse(audit.events()[0].metadata_json);
    REQUIRE(metadata["usage"]["truncated"] == true);
    REQUIRE(metadata["usage"]["data_dropped"] == true);
  });
}

TEST_CASE("dispatch publishes tool_after with permission_denied kind on the deny path", "[unit][tool][hook]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(noop_tool_def(), &noop_ok_handler).has_value());

    auto rules = deny_rule_set();
    permission::RecordingAuditSink audit;

    orangutan::hook::Bus bus;
    CaptureSink sink{"capture-deny"};
    bus.bind(sink, {orangutan::hook::Event::tool_before, orangutan::hook::Event::tool_after});

    auto ctx = make_hooked_ctx(io, rules, audit, &bus);
    auto result = co_await registry.dispatch("noop", R"({})", ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::permission_denied);

    REQUIRE(sink.captures().size() == 2);
    REQUIRE(sink.captures()[0].event == orangutan::hook::Event::tool_before);
    REQUIRE(sink.captures()[1].event == orangutan::hook::Event::tool_after);
    REQUIRE_FALSE(sink.captures()[1].succeeded);
    REQUIRE(sink.captures()[1].error_kind == "permission_denied");
    REQUIRE(sink.captures()[1].output_text.empty());
  });
}

TEST_CASE("dispatch publishes tool_after with the handler's error kind on handler failure", "[unit][tool][hook]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(noop_tool_def(), &noop_error_handler).has_value());

    auto rules = allow_rule_set();
    permission::RecordingAuditSink audit;

    orangutan::hook::Bus bus;
    CaptureSink sink{"capture-handler-err"};
    bus.bind(sink, {orangutan::hook::Event::tool_after});

    auto ctx = make_hooked_ctx(io, rules, audit, &bus);
    auto result = co_await registry.dispatch("noop", R"({})", ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::internal);

    REQUIRE(sink.captures().size() == 1);
    REQUIRE(sink.captures()[0].event == orangutan::hook::Event::tool_after);
    REQUIRE_FALSE(sink.captures()[0].succeeded);
    REQUIRE(sink.captures()[0].error_kind == "internal");
  });
}

TEST_CASE("dispatch does not publish any hook event for an unknown tool name", "[unit][tool][hook]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    auto rules = allow_rule_set("noop");
    permission::RecordingAuditSink audit;

    orangutan::hook::Bus bus;
    CaptureSink sink{"capture-unknown"};
    bus.bind(sink, {orangutan::hook::Event::tool_before, orangutan::hook::Event::tool_after});

    auto ctx = make_hooked_ctx(io, rules, audit, &bus);
    auto result = co_await registry.dispatch("missing", R"({})", ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::not_found);

    REQUIRE(sink.captures().empty());
  });
}

TEST_CASE("dispatch swallows sink errors — hook publish is advisory", "[unit][tool][hook]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(noop_tool_def(), &noop_ok_handler).has_value());

    auto rules = allow_rule_set();
    permission::RecordingAuditSink audit;

    orangutan::hook::Bus bus;
    FailingHookSink failing{"failing-1"};
    CaptureSink alive{"capture-1"};
    bus.bind(failing, {orangutan::hook::Event::tool_before, orangutan::hook::Event::tool_after});
    bus.bind(alive, {orangutan::hook::Event::tool_before, orangutan::hook::Event::tool_after});

    auto ctx = make_hooked_ctx(io, rules, audit, &bus);
    auto result = co_await registry.dispatch("noop", R"({})", ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text == "noop-ok");

    // The failing sink's advisory `tool_after` error does not stop later sinks.
    REQUIRE(alive.captures().size() == 2);
  });
}

TEST_CASE("null bus reproduces slice-21 behavior — no hook publish", "[unit][tool][hook]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(noop_tool_def(), &noop_ok_handler).has_value());

    auto rules = allow_rule_set();
    permission::RecordingAuditSink audit;

    auto ctx = make_hooked_ctx(io, rules, audit, /*bus=*/nullptr);
    auto result = co_await registry.dispatch("noop", R"({})", ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text == "noop-ok");
    // Audit row is still recorded — that side of the contract is unchanged.
    REQUIRE(audit.events().size() == 1);
    REQUIRE(audit.events()[0].outcome == permission::AuditOutcome::allow);
  });
}

TEST_CASE("ask short-circuit publishes tool_after with permission_denied + approval_required", "[unit][tool][hook]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(noop_tool_def(), &noop_ok_handler).has_value());

    auto rules = single_rule(permission::Rule{
        .verdict = permission::Verdict::ask,
        .tool_pattern = "noop",
    });
    permission::RecordingAuditSink audit;

    orangutan::hook::Bus bus;
    CaptureSink sink{"capture-ask"};
    bus.bind(sink, {orangutan::hook::Event::tool_before, orangutan::hook::Event::tool_after});

    auto ctx = make_hooked_ctx(io, rules, audit, &bus);
    auto result = co_await registry.dispatch("noop", R"({})", ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(context_has(result.error(), "reason", "approval_required"));

    REQUIRE(sink.captures().size() == 2);
    REQUIRE(sink.captures()[0].event == orangutan::hook::Event::tool_before);
    REQUIRE(sink.captures()[1].event == orangutan::hook::Event::tool_after);
    REQUIRE_FALSE(sink.captures()[1].succeeded);
    REQUIRE(sink.captures()[1].error_kind == "permission_denied");
  });
}

TEST_CASE("ask + broker rejection publishes tool_after with broker reason in the error kind", "[unit][tool][hook]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(noop_tool_def(), &noop_ok_handler).has_value());

    auto rules = single_rule(permission::Rule{
        .verdict = permission::Verdict::ask,
        .tool_pattern = "noop",
    });
    permission::RecordingAuditSink audit;

    auto broker = make_broker();
    const auto now = fixed_now();
    const auto exhausted = grant(broker, "noop", R"({})", "operator-1", now, /*replay_max=*/0);

    orangutan::hook::Bus bus;
    CaptureSink sink{"capture-broker-reject"};
    bus.bind(sink, {orangutan::hook::Event::tool_after});

    auto ctx = make_approval_ctx(io, rules, audit, &broker, &exhausted, now);
    ctx.bus = &bus;

    auto result = co_await registry.dispatch("noop", R"({})", ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(context_has(result.error(), "reason", "replay_exhausted"));

    REQUIRE(sink.captures().size() == 1);
    REQUIRE_FALSE(sink.captures()[0].succeeded);
    REQUIRE(sink.captures()[0].error_kind == "permission_denied");
  });
}

TEST_CASE("ask publishes permission_ask_rendered and proceeds when the operator approves",
          "[unit][tool][hook][approval]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(core::ToolDef::with_no_input("noop", "noop"), make_echo_handler()).has_value());
    auto rules = single_rule(permission::Rule{
        .verdict = permission::Verdict::ask,
        .tool_pattern = "noop",
        .replay_max = 3,
        .approval_ttl = std::chrono::seconds{120},
    });
    permission::RecordingAuditSink audit;
    auto broker = make_broker();
    const auto now = fixed_now();
    const std::string input = R"({"hello":"world"})";
    permission::ApprovalToken issued_token{};

    orangutan::hook::Bus bus;
    CaptureSink prompt{"operator-prompt"};
    orangutan::hook::HookDecision approved{};
    approved.reason = "operator_approved:operator-1";
    prompt.set_blocking_decision(approved);
    bus.bind(prompt, {orangutan::hook::Event::permission_ask_rendered});

    auto ctx = make_approval_ctx(io, rules, audit, &broker, /*token=*/nullptr, now);
    ctx.bus = &bus;
    ctx.approval_token_output = &issued_token;

    auto result = co_await registry.dispatch("noop", input, ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text == input);

    REQUIRE(prompt.captures().size() == 1);
    REQUIRE(prompt.captures()[0].event == orangutan::hook::Event::permission_ask_rendered);
    REQUIRE(prompt.captures()[0].tool_name == "noop");
    REQUIRE(prompt.captures()[0].input_json == input);
    REQUIRE(prompt.captures()[0].identity == "operator-1");
    REQUIRE(prompt.captures()[0].decision_reason == "rule #0 (ask: noop)");
    REQUIRE(prompt.captures()[0].replay_max == 3);
    REQUIRE(prompt.captures()[0].approval_ttl == std::chrono::seconds{120});

    REQUIRE(audit.events().size() == 1);
    const auto& event = audit.events()[0];
    REQUIRE(event.verdict == permission::Verdict::ask);
    REQUIRE(event.outcome == permission::AuditOutcome::approved);
    REQUIRE(event.reason == "rule #0 (ask: noop)");
    auto metadata = nlohmann::json::parse(event.metadata_json);
    REQUIRE(metadata["permission_ask_decisions"].size() == 1);
    REQUIRE(metadata["permission_ask_decisions"][0]["sink_id"] == "operator-prompt");
    REQUIRE(metadata["permission_ask_decisions"][0]["kind"] == "proceed");
    REQUIRE(metadata["permission_ask_decisions"][0]["reason"] == "operator_approved:operator-1");

    auto replay_ctx = make_approval_ctx(io, rules, audit, &broker, &issued_token, now);
    auto replay = co_await registry.dispatch("noop", input, replay_ctx);
    REQUIRE(replay.has_value());
    REQUIRE(replay->text == input);
    REQUIRE(audit.events().size() == 2);
    REQUIRE(audit.events()[1].outcome == permission::AuditOutcome::approved);
  });
}

TEST_CASE("ask rejects permission_ask_rendered proceed without operator identity", "[unit][tool][hook][approval]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    std::size_t handler_calls = 0;
    tool::Registry registry;
    REQUIRE(registry
                .add(core::ToolDef::with_no_input("noop", "noop"),
                     [&](std::string_view /*input*/,
                         tool::DispatchContext& /*ctx*/) -> async::Awaitable<core::Result<tool::Output>> {
                       ++handler_calls;
                       co_return tool::Output::text_only("should-not-run");
                     })
                .has_value());
    auto rules = single_rule(permission::Rule{.verdict = permission::Verdict::ask, .tool_pattern = "noop"});
    permission::RecordingAuditSink audit;
    auto broker = make_broker();

    orangutan::hook::Bus bus;
    CaptureSink prompt{"operator-prompt"};
    bus.bind(prompt, {orangutan::hook::Event::permission_ask_rendered});

    auto ctx = make_approval_ctx(io, rules, audit, &broker, /*token=*/nullptr, fixed_now());
    ctx.bus = &bus;

    auto result = co_await registry.dispatch("noop", R"({"empty_proceed":true})", ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(context_has(result.error(), "reason", "operator_denied"));
    REQUIRE(context_has(result.error(), "decision_kind", "proceed"));
    REQUIRE(context_has(result.error(), "hook_reason", "permission_ask_missing_operator_reason"));
    REQUIRE(handler_calls == 0);
    REQUIRE(broker.outstanding_grants() == 0);

    REQUIRE(audit.events().size() == 1);
    const auto& event = audit.events()[0];
    REQUIRE(event.verdict == permission::Verdict::ask);
    REQUIRE(event.outcome == permission::AuditOutcome::rejected);
    REQUIRE(event.reason == "operator_denied");
    auto metadata = nlohmann::json::parse(event.metadata_json);
    REQUIRE(metadata["permission_ask_decisions"].size() == 1);
    REQUIRE(metadata["permission_ask_decisions"][0]["sink_id"] == "operator-prompt");
    REQUIRE(metadata["permission_ask_decisions"][0]["kind"] == "proceed");
    REQUIRE(metadata["permission_ask_decisions"][0]["reason"] == "");
  });
}

TEST_CASE("ask publishes permission_ask_rendered and rejects when the operator vetoes",
          "[unit][tool][hook][approval]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    std::size_t handler_calls = 0;
    tool::Registry registry;
    REQUIRE(registry
                .add(core::ToolDef::with_no_input("noop", "noop"),
                     [&](std::string_view /*input*/,
                         tool::DispatchContext& /*ctx*/) -> async::Awaitable<core::Result<tool::Output>> {
                       ++handler_calls;
                       co_return tool::Output::text_only("should-not-run");
                     })
                .has_value());
    auto rules = single_rule(permission::Rule{.verdict = permission::Verdict::ask, .tool_pattern = "noop"});
    permission::RecordingAuditSink audit;
    auto broker = make_broker();

    orangutan::hook::HookDecision veto{};
    veto.kind = orangutan::hook::HookDecisionKind::veto;
    veto.reason = "operator_approved:false";

    orangutan::hook::Bus bus;
    CaptureSink prompt{"operator-prompt"};
    prompt.set_blocking_decision(veto);
    bus.bind(prompt, {orangutan::hook::Event::permission_ask_rendered});

    auto ctx = make_approval_ctx(io, rules, audit, &broker, /*token=*/nullptr, fixed_now());
    ctx.bus = &bus;

    auto result = co_await registry.dispatch("noop", R"({"denied":true})", ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(context_has(result.error(), "reason", "operator_denied"));
    REQUIRE(context_has(result.error(), "hook_reason", "operator_approved:false"));
    REQUIRE(handler_calls == 0);
    REQUIRE(broker.outstanding_grants() == 0);

    REQUIRE(audit.events().size() == 1);
    const auto& event = audit.events()[0];
    REQUIRE(event.verdict == permission::Verdict::ask);
    REQUIRE(event.outcome == permission::AuditOutcome::rejected);
    REQUIRE(event.reason == "operator_denied");
    auto metadata = nlohmann::json::parse(event.metadata_json);
    REQUIRE(metadata["permission_ask_decisions"].size() == 1);
    REQUIRE(metadata["permission_ask_decisions"][0]["sink_id"] == "operator-prompt");
    REQUIRE(metadata["permission_ask_decisions"][0]["kind"] == "veto");
    REQUIRE(metadata["permission_ask_decisions"][0]["reason"] == "operator_approved:false");
  });
}

// ---------------------------------------------------------------------------
// slice 25 — `tool_dispatched` + `tool_error` publish on top of the slice-22
// bookend pair.
//
// `tool_dispatched` fires exactly once, between audit success and the
// handler co_await, on the paths where the handler will actually run
// (allow OR ask-approved). Sinks subscribed to it skip the
// deny/short-circuit/reject branches without filtering.
//
// `tool_error` fires alongside `tool_after` whenever the dispatch result
// is an error (handler failure, permission deny, broker rejection, audit
// error, ask short-circuit). Sinks that only care about failures avoid
// the `tool_after::succeeded` filter dance.
//
// Both events stay advisory — sink errors are captured but do not change
// the dispatch result.

TEST_CASE("dispatch publishes tool_dispatched on the allow path with verdict=allow", "[unit][tool][hook]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(noop_tool_def(), &noop_ok_handler).has_value());

    auto rules = allow_rule_set();
    permission::RecordingAuditSink audit;

    orangutan::hook::Bus bus;
    CaptureSink sink{"capture-dispatched-allow"};
    bus.bind(sink,
             {orangutan::hook::Event::tool_before,
              orangutan::hook::Event::tool_dispatched,
              orangutan::hook::Event::tool_after});

    auto ctx = make_hooked_ctx(io, rules, audit, &bus);
    auto result = co_await registry.dispatch("noop", R"({"k":1})", ctx);
    REQUIRE(result.has_value());

    REQUIRE(sink.captures().size() == 3);
    REQUIRE(sink.captures()[0].event == orangutan::hook::Event::tool_before);
    REQUIRE(sink.captures()[1].event == orangutan::hook::Event::tool_dispatched);
    REQUIRE(sink.captures()[1].tool_name == "noop");
    REQUIRE(sink.captures()[1].identity == "operator-1");
    REQUIRE(sink.captures()[1].verdict == "allow");
    REQUIRE(sink.captures()[2].event == orangutan::hook::Event::tool_after);
    REQUIRE(sink.captures()[2].succeeded);
  });
}

TEST_CASE("dispatch does NOT publish tool_dispatched on the deny path", "[unit][tool][hook]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(noop_tool_def(), &noop_ok_handler).has_value());

    auto rules = deny_rule_set();
    permission::RecordingAuditSink audit;

    orangutan::hook::Bus bus;
    CaptureSink sink{"capture-dispatched-deny"};
    bus.bind(sink, {orangutan::hook::Event::tool_dispatched, orangutan::hook::Event::tool_after});

    auto ctx = make_hooked_ctx(io, rules, audit, &bus);
    auto result = co_await registry.dispatch("noop", R"({})", ctx);
    REQUIRE_FALSE(result.has_value());

    // tool_dispatched never fires; only tool_after.
    REQUIRE(sink.captures().size() == 1);
    REQUIRE(sink.captures()[0].event == orangutan::hook::Event::tool_after);
  });
}

TEST_CASE("dispatch does NOT publish tool_dispatched on the ask short-circuit path", "[unit][tool][hook]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(noop_tool_def(), &noop_ok_handler).has_value());

    auto rules = single_rule(permission::Rule{
        .verdict = permission::Verdict::ask,
        .tool_pattern = "noop",
    });
    permission::RecordingAuditSink audit;

    orangutan::hook::Bus bus;
    CaptureSink sink{"capture-dispatched-ask-short"};
    bus.bind(sink, {orangutan::hook::Event::tool_dispatched, orangutan::hook::Event::tool_after});

    auto ctx = make_hooked_ctx(io, rules, audit, &bus);  // no broker → short-circuit.
    auto result = co_await registry.dispatch("noop", R"({})", ctx);
    REQUIRE_FALSE(result.has_value());

    REQUIRE(sink.captures().size() == 1);
    REQUIRE(sink.captures()[0].event == orangutan::hook::Event::tool_after);
  });
}

TEST_CASE("dispatch publishes tool_dispatched with verdict=ask on the ask-approved path", "[unit][tool][hook]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(core::ToolDef::with_no_input("noop", "noop"), make_echo_handler()).has_value());
    auto rules = single_rule(permission::Rule{.verdict = permission::Verdict::ask, .tool_pattern = "noop"});
    permission::RecordingAuditSink audit;

    auto broker = make_broker();
    const auto now = fixed_now();
    const std::string_view input = R"({"hello":"world"})";
    const auto token = grant(broker, "noop", input, "operator-1", now);

    orangutan::hook::Bus bus;
    CaptureSink sink{"capture-dispatched-ask-approved"};
    bus.bind(sink, {orangutan::hook::Event::tool_dispatched, orangutan::hook::Event::tool_after});

    auto ctx = make_approval_ctx(io, rules, audit, &broker, &token, now);
    ctx.bus = &bus;

    auto result = co_await registry.dispatch("noop", input, ctx);
    REQUIRE(result.has_value());

    REQUIRE(sink.captures().size() == 2);
    REQUIRE(sink.captures()[0].event == orangutan::hook::Event::tool_dispatched);
    // The verdict wire spelling is the rule's verdict (`ask`); the
    // approval-broker promotion lives on the audit row's `outcome`
    // (`approved`), not on the dispatched-event verdict.
    REQUIRE(sink.captures()[0].verdict == "ask");
    REQUIRE(sink.captures()[1].event == orangutan::hook::Event::tool_after);
    REQUIRE(sink.captures()[1].succeeded);
  });
}

TEST_CASE("dispatch does NOT publish tool_dispatched on broker rejection", "[unit][tool][hook]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(noop_tool_def(), &noop_ok_handler).has_value());

    auto rules = single_rule(permission::Rule{.verdict = permission::Verdict::ask, .tool_pattern = "noop"});
    permission::RecordingAuditSink audit;

    auto broker = make_broker();
    const auto now = fixed_now();
    const auto exhausted = grant(broker, "noop", R"({})", "operator-1", now, /*replay_max=*/0);

    orangutan::hook::Bus bus;
    CaptureSink sink{"capture-dispatched-broker-reject"};
    bus.bind(sink, {orangutan::hook::Event::tool_dispatched, orangutan::hook::Event::tool_after});

    auto ctx = make_approval_ctx(io, rules, audit, &broker, &exhausted, now);
    ctx.bus = &bus;

    auto result = co_await registry.dispatch("noop", R"({})", ctx);
    REQUIRE_FALSE(result.has_value());

    REQUIRE(sink.captures().size() == 1);
    REQUIRE(sink.captures()[0].event == orangutan::hook::Event::tool_after);
  });
}

TEST_CASE("dispatch publishes tool_error on handler failure", "[unit][tool][hook]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(noop_tool_def(), &noop_error_handler).has_value());

    auto rules = allow_rule_set();
    permission::RecordingAuditSink audit;

    orangutan::hook::Bus bus;
    CaptureSink sink{"capture-error-handler"};
    bus.bind(sink, {orangutan::hook::Event::tool_error, orangutan::hook::Event::tool_after});

    auto ctx = make_hooked_ctx(io, rules, audit, &bus);
    auto result = co_await registry.dispatch("noop", R"({})", ctx);
    REQUIRE_FALSE(result.has_value());

    REQUIRE(sink.captures().size() == 2);
    REQUIRE(sink.captures()[0].event == orangutan::hook::Event::tool_error);
    REQUIRE(sink.captures()[0].error_kind == "internal");
    REQUIRE(sink.captures()[0].error_message == "handler exploded");
    REQUIRE(sink.captures()[0].tool_name == "noop");
    REQUIRE(sink.captures()[0].identity == "operator-1");
    REQUIRE(sink.captures()[1].event == orangutan::hook::Event::tool_after);
    REQUIRE_FALSE(sink.captures()[1].succeeded);
  });
}

TEST_CASE("dispatch publishes tool_error on permission deny", "[unit][tool][hook]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(noop_tool_def(), &noop_ok_handler).has_value());

    auto rules = deny_rule_set();
    permission::RecordingAuditSink audit;

    orangutan::hook::Bus bus;
    CaptureSink sink{"capture-error-deny"};
    bus.bind(sink, {orangutan::hook::Event::tool_error});

    auto ctx = make_hooked_ctx(io, rules, audit, &bus);
    auto result = co_await registry.dispatch("noop", R"({})", ctx);
    REQUIRE_FALSE(result.has_value());

    REQUIRE(sink.captures().size() == 1);
    REQUIRE(sink.captures()[0].event == orangutan::hook::Event::tool_error);
    REQUIRE(sink.captures()[0].error_kind == "permission_denied");
  });
}

TEST_CASE("dispatch publishes tool_error on ask short-circuit", "[unit][tool][hook]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(noop_tool_def(), &noop_ok_handler).has_value());

    auto rules = single_rule(permission::Rule{
        .verdict = permission::Verdict::ask,
        .tool_pattern = "noop",
    });
    permission::RecordingAuditSink audit;

    orangutan::hook::Bus bus;
    CaptureSink sink{"capture-error-ask-short"};
    bus.bind(sink, {orangutan::hook::Event::tool_error});

    auto ctx = make_hooked_ctx(io, rules, audit, &bus);
    auto result = co_await registry.dispatch("noop", R"({})", ctx);
    REQUIRE_FALSE(result.has_value());

    REQUIRE(sink.captures().size() == 1);
    REQUIRE(sink.captures()[0].event == orangutan::hook::Event::tool_error);
    REQUIRE(sink.captures()[0].error_kind == "permission_denied");
  });
}

TEST_CASE("dispatch publishes tool_error on broker rejection with broker reason in message", "[unit][tool][hook]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(noop_tool_def(), &noop_ok_handler).has_value());

    auto rules = single_rule(permission::Rule{.verdict = permission::Verdict::ask, .tool_pattern = "noop"});
    permission::RecordingAuditSink audit;

    auto broker = make_broker();
    const auto now = fixed_now();
    const auto exhausted = grant(broker, "noop", R"({})", "operator-1", now, /*replay_max=*/0);

    orangutan::hook::Bus bus;
    CaptureSink sink{"capture-error-broker"};
    bus.bind(sink, {orangutan::hook::Event::tool_error});

    auto ctx = make_approval_ctx(io, rules, audit, &broker, &exhausted, now);
    ctx.bus = &bus;

    auto result = co_await registry.dispatch("noop", R"({})", ctx);
    REQUIRE_FALSE(result.has_value());

    REQUIRE(sink.captures().size() == 1);
    REQUIRE(sink.captures()[0].event == orangutan::hook::Event::tool_error);
    REQUIRE(sink.captures()[0].error_kind == "permission_denied");
  });
}

TEST_CASE("dispatch does NOT publish tool_error on the allow happy path", "[unit][tool][hook]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(noop_tool_def(), &noop_ok_handler).has_value());

    auto rules = allow_rule_set();
    permission::RecordingAuditSink audit;

    orangutan::hook::Bus bus;
    CaptureSink sink{"capture-error-none"};
    bus.bind(sink, {orangutan::hook::Event::tool_error, orangutan::hook::Event::tool_after});

    auto ctx = make_hooked_ctx(io, rules, audit, &bus);
    auto result = co_await registry.dispatch("noop", R"({})", ctx);
    REQUIRE(result.has_value());

    REQUIRE(sink.captures().size() == 1);
    REQUIRE(sink.captures()[0].event == orangutan::hook::Event::tool_after);
    REQUIRE(sink.captures()[0].succeeded);
  });
}

TEST_CASE("dispatch publishes tool_dispatched + tool_error + tool_after in the right order across the four events",
          "[unit][tool][hook]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(registry.add(noop_tool_def(), &noop_error_handler).has_value());

    auto rules = allow_rule_set();
    permission::RecordingAuditSink audit;

    orangutan::hook::Bus bus;
    CaptureSink sink{"capture-order"};
    bus.bind(sink,
             {orangutan::hook::Event::tool_before,
              orangutan::hook::Event::tool_dispatched,
              orangutan::hook::Event::tool_error,
              orangutan::hook::Event::tool_after});

    auto ctx = make_hooked_ctx(io, rules, audit, &bus);
    auto result = co_await registry.dispatch("noop", R"({})", ctx);
    REQUIRE_FALSE(result.has_value());

    // before → dispatched (handler about to run) → error (handler returned
    // an error) → after.
    REQUIRE(sink.captures().size() == 4);
    REQUIRE(sink.captures()[0].event == orangutan::hook::Event::tool_before);
    REQUIRE(sink.captures()[1].event == orangutan::hook::Event::tool_dispatched);
    REQUIRE(sink.captures()[1].verdict == "allow");
    REQUIRE(sink.captures()[2].event == orangutan::hook::Event::tool_error);
    REQUIRE(sink.captures()[2].error_kind == "internal");
    REQUIRE(sink.captures()[3].event == orangutan::hook::Event::tool_after);
    REQUIRE_FALSE(sink.captures()[3].succeeded);
  });
}
