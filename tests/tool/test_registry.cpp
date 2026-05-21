// tests/tool/test_registry.cpp — registry add/find/catalog/dispatch coverage.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/cancellation_type.hpp>
#include <asio/co_spawn.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <oran/async.hpp>
#include <oran/core/capability.hpp>
#include <oran/core/error.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/hook.hpp>
#include <oran/permission.hpp>
#include <oran/tool.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace permission = orangutan::permission;
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

async::Awaitable<core::Result<tool::Output>> echo_handler(std::string_view input, tool::DispatchContext& /*ctx*/) {
  co_return tool::Output{.text = std::string{input}};
}

tool::Handler make_echo_handler() {
  return &echo_handler;
}

permission::RuleSet single_rule(permission::Rule rule) {
  permission::RuleSet rs;
  rs.add(std::move(rule));
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
        .name = "needs.read",
        .description = "tool needing read_file",
        .input_schema_json = "{}",
        .required_capabilities = {core::Capability::read_file},
    };
    REQUIRE(registry.add(std::move(def), make_echo_handler()).has_value());

    // Capability that does NOT match the tool's declared `read_file` — under
    // `Mode::strict` the call falls through to the mode default (deny).
    auto wrong_cap = single_rule(permission::Rule{
        .verdict = permission::Verdict::allow,
        .tool_pattern = "needs.*",
        .capability = core::Capability::write_file,
    });
    permission::RecordingAuditSink sink_wrong;
    auto ctx_wrong = make_ctx(io, wrong_cap, sink_wrong, permission::Mode::strict);
    auto wrong = co_await registry.dispatch("needs.read", "{}", ctx_wrong);
    REQUIRE_FALSE(wrong.has_value());
    REQUIRE(wrong.error().kind() == core::ErrorKind::permission_denied);

    // Capability that DOES match — the rule fires and allow flows through.
    auto right_cap = single_rule(permission::Rule{
        .verdict = permission::Verdict::allow,
        .tool_pattern = "needs.*",
        .capability = core::Capability::read_file,
    });
    permission::RecordingAuditSink sink_right;
    auto ctx_right = make_ctx(io, right_cap, sink_right, permission::Mode::strict);
    auto right = co_await registry.dispatch("needs.read", "{}", ctx_right);
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

TEST_CASE("register_builtins seeds the file tool catalog", "[unit][tool][builtins]") {
  tool::Registry registry;
  REQUIRE(tool::register_builtins(registry).has_value());
  const auto catalog = registry.catalog();
  REQUIRE(catalog.size() == 6);
  REQUIRE(catalog[0].name == tool::kFileReadName);
  REQUIRE(catalog[1].name == tool::kFileWriteName);
  REQUIRE(catalog[2].name == tool::kFileEditName);
  REQUIRE(catalog[3].name == tool::kFileSearchName);
  REQUIRE(catalog[4].name == tool::kDirectoryListName);
  REQUIRE(catalog[5].name == tool::kFileDeleteName);
}

TEST_CASE("file.read happy path returns the file contents verbatim", "[unit][tool][file_read]") {
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
    REQUIRE(result->text == "hello, slice 17");
    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::allow);
  });
}

TEST_CASE("file.read rejects malformed input as invalid_argument", "[unit][tool][file_read]") {
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

    auto bad_json = co_await registry.dispatch(tool::kFileReadName, "{not-json}", ctx);
    REQUIRE_FALSE(bad_json.has_value());
    REQUIRE(bad_json.error().kind() == core::ErrorKind::invalid_argument);

    auto missing_path = co_await registry.dispatch(tool::kFileReadName, R"({"target":"x"})", ctx);
    REQUIRE_FALSE(missing_path.has_value());
    REQUIRE(missing_path.error().kind() == core::ErrorKind::invalid_argument);

    auto non_string = co_await registry.dispatch(tool::kFileReadName, R"({"path":42})", ctx);
    REQUIRE_FALSE(non_string.has_value());
    REQUIRE(non_string.error().kind() == core::ErrorKind::invalid_argument);

    // All three calls were `allow`-verdict permission decisions — audit
    // recorded one row each.
    REQUIRE(sink.events().size() == 3);
    for (const auto& event : sink.events()) {
      REQUIRE(event.outcome == permission::AuditOutcome::allow);
    }
  });
}

TEST_CASE("file.read returns not_found when the path does not exist", "[unit][tool][file_read]") {
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

TEST_CASE("file.write happy path writes the bytes verbatim and reports the size", "[unit][tool][file_write]") {
  TempFile file{"happy-write"};

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_write(registry).has_value());
    auto rules = write_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    nlohmann::json input{{"path", file.string()}, {"content", "hello, slice 18"}};
    auto result = co_await registry.dispatch(tool::kFileWriteName, input.dump(), ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text.contains("wrote 15 bytes"));
    REQUIRE(result->text.contains(file.string()));
    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::allow);
  });

  REQUIRE(slurp(file.string()) == "hello, slice 18");
}

TEST_CASE("file.write default mode overwrites an existing file", "[unit][tool][file_write]") {
  TempFile file{"overwrite"};
  file.write("original");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_write(registry).has_value());
    auto rules = write_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    nlohmann::json input{{"path", file.string()}, {"content", "replaced"}};
    auto result = co_await registry.dispatch(tool::kFileWriteName, input.dump(), ctx);
    REQUIRE(result.has_value());
  });

  REQUIRE(slurp(file.string()) == "replaced");
}

TEST_CASE("file.write mode=append appends to existing content", "[unit][tool][file_write]") {
  TempFile file{"append"};
  file.write("head");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_write(registry).has_value());
    auto rules = write_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    nlohmann::json input{{"path", file.string()}, {"content", "-tail"}, {"mode", "append"}};
    auto result = co_await registry.dispatch(tool::kFileWriteName, input.dump(), ctx);
    REQUIRE(result.has_value());
  });

  REQUIRE(slurp(file.string()) == "head-tail");
}

TEST_CASE("file.write mode=fail_if_exists returns conflict when the path already exists", "[unit][tool][file_write]") {
  TempFile file{"exists"};
  file.write("present");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_write(registry).has_value());
    auto rules = write_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    nlohmann::json input{{"path", file.string()}, {"content", "replacement"}, {"mode", "fail_if_exists"}};
    auto result = co_await registry.dispatch(tool::kFileWriteName, input.dump(), ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::conflict);
  });

  REQUIRE(slurp(file.string()) == "present");
}

TEST_CASE("file.write create_parents=true creates missing directories", "[unit][tool][file_write]") {
  const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const auto base = std::filesystem::temp_directory_path() / ("oran-tool-create-parents-" + stamp);
  const auto target = base / "nested" / "deeper" / "out.txt";

  test::run_async([&target](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_write(registry).has_value());
    auto rules = write_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    nlohmann::json input{{"path", target.string()}, {"content", "deep"}, {"create_parents", true}};
    auto result = co_await registry.dispatch(tool::kFileWriteName, input.dump(), ctx);
    REQUIRE(result.has_value());
  });

  REQUIRE(std::filesystem::exists(target));
  REQUIRE(slurp(target.string()) == "deep");

  std::error_code ec;
  std::filesystem::remove_all(base, ec);
}

TEST_CASE("file.write rejects malformed input as invalid_argument", "[unit][tool][file_write]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_write(registry).has_value());
    auto rules = write_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    auto bad_json = co_await registry.dispatch(tool::kFileWriteName, "{not-json}", ctx);
    REQUIRE_FALSE(bad_json.has_value());
    REQUIRE(bad_json.error().kind() == core::ErrorKind::invalid_argument);

    auto missing_path = co_await registry.dispatch(tool::kFileWriteName, R"({"content":"x"})", ctx);
    REQUIRE_FALSE(missing_path.has_value());
    REQUIRE(missing_path.error().kind() == core::ErrorKind::invalid_argument);

    auto missing_content = co_await registry.dispatch(tool::kFileWriteName, R"({"path":"/tmp/x"})", ctx);
    REQUIRE_FALSE(missing_content.has_value());
    REQUIRE(missing_content.error().kind() == core::ErrorKind::invalid_argument);

    auto wrong_path = co_await registry.dispatch(tool::kFileWriteName, R"({"path":42,"content":"x"})", ctx);
    REQUIRE_FALSE(wrong_path.has_value());
    REQUIRE(wrong_path.error().kind() == core::ErrorKind::invalid_argument);

    auto wrong_content = co_await registry.dispatch(tool::kFileWriteName, R"({"path":"/tmp/x","content":3})", ctx);
    REQUIRE_FALSE(wrong_content.has_value());
    REQUIRE(wrong_content.error().kind() == core::ErrorKind::invalid_argument);

    auto wrong_mode_type =
        co_await registry.dispatch(tool::kFileWriteName, R"({"path":"/tmp/x","content":"y","mode":3})", ctx);
    REQUIRE_FALSE(wrong_mode_type.has_value());
    REQUIRE(wrong_mode_type.error().kind() == core::ErrorKind::invalid_argument);

    auto unknown_mode =
        co_await registry.dispatch(tool::kFileWriteName, R"({"path":"/tmp/x","content":"y","mode":"bogus"})", ctx);
    REQUIRE_FALSE(unknown_mode.has_value());
    REQUIRE(unknown_mode.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(context_has(unknown_mode.error(), "value", "bogus"));

    auto wrong_create_parents = co_await registry.dispatch(tool::kFileWriteName,
                                                           R"({"path":"/tmp/x","content":"y","create_parents":"yes"})",
                                                           ctx);
    REQUIRE_FALSE(wrong_create_parents.has_value());
    REQUIRE(wrong_create_parents.error().kind() == core::ErrorKind::invalid_argument);

    // Every rejected call passed the permission gate, so audit recorded one
    // `allow` row per attempt (8 calls).
    REQUIRE(sink.events().size() == 8);
    for (const auto& event : sink.events()) {
      REQUIRE(event.outcome == permission::AuditOutcome::allow);
    }
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

TEST_CASE("file.edit happy path replaces a unique occurrence and reports a count", "[unit][tool][file_edit]") {
  TempFile file{"edit-unique"};
  file.write("alpha beta gamma");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_edit(registry).has_value());
    auto rules = edit_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    nlohmann::json input{{"path", file.string()}, {"old_string", "beta"}, {"new_string", "BETA"}};
    auto result = co_await registry.dispatch(tool::kFileEditName, input.dump(), ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text.contains("1 replacement"));
    REQUIRE_FALSE(result->text.contains("replacements"));
    REQUIRE(result->text.contains(file.string()));
    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::allow);
  });

  REQUIRE(slurp(file.string()) == "alpha BETA gamma");
}

TEST_CASE("file.edit replace_all=true rewrites every occurrence", "[unit][tool][file_edit]") {
  TempFile file{"edit-replace-all"};
  file.write("foo bar foo baz foo");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_edit(registry).has_value());
    auto rules = edit_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    nlohmann::json input{{"path", file.string()}, {"old_string", "foo"}, {"new_string", "qux"}, {"replace_all", true}};
    auto result = co_await registry.dispatch(tool::kFileEditName, input.dump(), ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text.contains("3 replacements"));
  });

  REQUIRE(slurp(file.string()) == "qux bar qux baz qux");
}

TEST_CASE("file.edit returns conflict when old_string is not unique and replace_all is false",
          "[unit][tool][file_edit]") {
  TempFile file{"edit-ambiguous"};
  file.write("dup dup dup");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_edit(registry).has_value());
    auto rules = edit_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    nlohmann::json input{{"path", file.string()}, {"old_string", "dup"}, {"new_string", "x"}};
    auto result = co_await registry.dispatch(tool::kFileEditName, input.dump(), ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::conflict);
    REQUIRE(context_has(result.error(), "match_count", "3"));
  });

  // File must be untouched when the call refuses.
  REQUIRE(slurp(file.string()) == "dup dup dup");
}

TEST_CASE("file.edit returns not_found when old_string does not appear", "[unit][tool][file_edit]") {
  TempFile file{"edit-missing-substring"};
  file.write("hello world");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_edit(registry).has_value());
    auto rules = edit_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    nlohmann::json input{{"path", file.string()}, {"old_string", "absent"}, {"new_string", "present"}};
    auto result = co_await registry.dispatch(tool::kFileEditName, input.dump(), ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::not_found);
  });

  REQUIRE(slurp(file.string()) == "hello world");
}

TEST_CASE("file.edit propagates not_found when the file is missing", "[unit][tool][file_edit]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_edit(registry).has_value());
    auto rules = edit_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto path = std::string{"/tmp/oran-tool-edit-missing-"} +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    nlohmann::json input{{"path", path}, {"old_string", "x"}, {"new_string", "y"}};
    auto result = co_await registry.dispatch(tool::kFileEditName, input.dump(), ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::not_found);
  });
}

TEST_CASE("file.edit rejects malformed input as invalid_argument", "[unit][tool][file_edit]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_edit(registry).has_value());
    auto rules = edit_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    auto bad_json = co_await registry.dispatch(tool::kFileEditName, "{not-json}", ctx);
    REQUIRE_FALSE(bad_json.has_value());
    REQUIRE(bad_json.error().kind() == core::ErrorKind::invalid_argument);

    auto missing_path = co_await registry.dispatch(tool::kFileEditName, R"({"old_string":"a","new_string":"b"})", ctx);
    REQUIRE_FALSE(missing_path.has_value());
    REQUIRE(missing_path.error().kind() == core::ErrorKind::invalid_argument);

    auto missing_old = co_await registry.dispatch(tool::kFileEditName, R"({"path":"/tmp/x","new_string":"b"})", ctx);
    REQUIRE_FALSE(missing_old.has_value());
    REQUIRE(missing_old.error().kind() == core::ErrorKind::invalid_argument);

    auto missing_new = co_await registry.dispatch(tool::kFileEditName, R"({"path":"/tmp/x","old_string":"a"})", ctx);
    REQUIRE_FALSE(missing_new.has_value());
    REQUIRE(missing_new.error().kind() == core::ErrorKind::invalid_argument);

    auto wrong_path =
        co_await registry.dispatch(tool::kFileEditName, R"({"path":1,"old_string":"a","new_string":"b"})", ctx);
    REQUIRE_FALSE(wrong_path.has_value());
    REQUIRE(wrong_path.error().kind() == core::ErrorKind::invalid_argument);

    auto wrong_replace_all =
        co_await registry.dispatch(tool::kFileEditName,
                                   R"({"path":"/tmp/x","old_string":"a","new_string":"b","replace_all":"yes"})",
                                   ctx);
    REQUIRE_FALSE(wrong_replace_all.has_value());
    REQUIRE(wrong_replace_all.error().kind() == core::ErrorKind::invalid_argument);

    auto empty_old =
        co_await registry.dispatch(tool::kFileEditName, R"({"path":"/tmp/x","old_string":"","new_string":"b"})", ctx);
    REQUIRE_FALSE(empty_old.has_value());
    REQUIRE(empty_old.error().kind() == core::ErrorKind::invalid_argument);

    auto identical =
        co_await registry.dispatch(tool::kFileEditName, R"({"path":"/tmp/x","old_string":"a","new_string":"a"})", ctx);
    REQUIRE_FALSE(identical.has_value());
    REQUIRE(identical.error().kind() == core::ErrorKind::invalid_argument);

    // Every rejected call passed the permission gate, so audit recorded one
    // `allow` row per attempt (8 calls).
    REQUIRE(sink.events().size() == 8);
    for (const auto& event : sink.events()) {
      REQUIRE(event.outcome == permission::AuditOutcome::allow);
    }
  });
}

namespace {

class TempDir {
public:
  explicit TempDir(std::string suffix)
      : path_(std::filesystem::temp_directory_path() /
              ("oran-tool-search-" + std::move(suffix) + "-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
    std::filesystem::create_directories(path_);
  }

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  void write_file(const std::filesystem::path& relative, std::string_view contents) const {
    const auto target = path_ / relative;
    std::filesystem::create_directories(target.parent_path());
    std::ofstream out{target, std::ios::binary};
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  }

  [[nodiscard]] std::string string() const {
    return path_.string();
  }

  [[nodiscard]] std::filesystem::path child(const std::filesystem::path& relative) const {
    return path_ / relative;
  }

private:
  std::filesystem::path path_;
};

permission::RuleSet search_rule_set() {
  return single_rule(permission::Rule{
      .verdict = permission::Verdict::allow,
      .tool_pattern = std::string{tool::kFileSearchName},
      .capability = core::Capability::read_file,
  });
}

}  // namespace

TEST_CASE("register_file_search advertises a `read_file` capability and a path/pattern schema",
          "[unit][tool][file_search]") {
  tool::Registry registry;
  REQUIRE(tool::register_file_search(registry).has_value());
  REQUIRE(registry.size() == 1);
  const auto* def = registry.find(tool::kFileSearchName);
  REQUIRE(def != nullptr);
  REQUIRE(def->required_capabilities.size() == 1);
  REQUIRE(def->required_capabilities[0] == core::Capability::read_file);
  REQUIRE(def->input_schema_json.contains("\"path\""));
  REQUIRE(def->input_schema_json.contains("\"pattern\""));
  REQUIRE(def->input_schema_json.contains("\"max_matches\""));
  REQUIRE(def->input_schema_json.contains("\"include_hidden\""));
}

TEST_CASE("file.search happy path on a single file reports path:line:text", "[unit][tool][file_search]") {
  TempFile file{"search-single"};
  file.write("alpha\nNEEDLE here\ngamma\n");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_search(registry).has_value());
    auto rules = search_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto input = std::string{R"({"path":")"} + file.string() + R"(","pattern":"NEEDLE"})";
    auto result = co_await registry.dispatch(tool::kFileSearchName, input, ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text.contains(file.string() + ":2:NEEDLE here"));
    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::allow);
  });
}

TEST_CASE("file.search walks a directory recursively and reports each match", "[unit][tool][file_search]") {
  TempDir dir{"recursive"};
  dir.write_file("a.txt", "TARGET on line one\n");
  dir.write_file("sub/b.txt", "filler\nTARGET on line two\n");
  dir.write_file("sub/deep/c.txt", "TARGET\nfiller\nTARGET again\n");

  test::run_async([&dir](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_search(registry).has_value());
    auto rules = search_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto input = std::string{R"({"path":")"} + dir.string() + R"(","pattern":"TARGET"})";
    auto result = co_await registry.dispatch(tool::kFileSearchName, input, ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text.contains(dir.child("a.txt").string() + ":1:TARGET on line one"));
    REQUIRE(result->text.contains(dir.child("sub/b.txt").string() + ":2:TARGET on line two"));
    REQUIRE(result->text.contains(dir.child("sub/deep/c.txt").string() + ":1:TARGET"));
    REQUIRE(result->text.contains(dir.child("sub/deep/c.txt").string() + ":3:TARGET again"));
  });
}

TEST_CASE("file.search caps results at max_matches and reports truncation", "[unit][tool][file_search]") {
  TempDir dir{"truncate"};
  dir.write_file("a.txt", "X\nX\nX\nX\nX\n");

  test::run_async([&dir](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_search(registry).has_value());
    auto rules = search_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto input = std::string{R"({"path":")"} + dir.string() + R"(","pattern":"X","max_matches":3})";
    auto result = co_await registry.dispatch(tool::kFileSearchName, input, ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text.contains("(truncated; matches capped at 3)"));
    // Three match lines plus one truncation line, joined by '\n' — three internal separators.
    const auto newline_count = std::ranges::count(std::string_view{result->text}, '\n');
    REQUIRE(newline_count == 3);
  });
}

TEST_CASE("file.search skips binary files (NUL bytes in the first 8 KiB)", "[unit][tool][file_search]") {
  TempDir dir{"binary"};
  std::string binary_content;
  binary_content.append("\0\0\0NEEDLE", 9U);
  binary_content.append(std::string(100, 'x'));
  dir.write_file("a.bin", std::string_view{binary_content});
  dir.write_file("b.txt", "NEEDLE here\n");

  test::run_async([&dir](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_search(registry).has_value());
    auto rules = search_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto input = std::string{R"({"path":")"} + dir.string() + R"(","pattern":"NEEDLE"})";
    auto result = co_await registry.dispatch(tool::kFileSearchName, input, ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text.contains(dir.child("b.txt").string()));
    REQUIRE_FALSE(result->text.contains(dir.child("a.bin").string()));
  });
}

TEST_CASE("file.search default include_hidden=false skips dot-prefixed files and directories",
          "[unit][tool][file_search]") {
  TempDir dir{"hidden"};
  dir.write_file("visible.txt", "needle here\n");
  dir.write_file(".hidden.txt", "needle here\n");
  dir.write_file(".cache/c.txt", "needle here\n");

  test::run_async([&dir](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_search(registry).has_value());
    auto rules = search_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto default_input = std::string{R"({"path":")"} + dir.string() + R"(","pattern":"needle"})";
    auto default_result = co_await registry.dispatch(tool::kFileSearchName, default_input, ctx);
    REQUIRE(default_result.has_value());
    REQUIRE(default_result->text.contains(dir.child("visible.txt").string()));
    REQUIRE_FALSE(default_result->text.contains(".hidden.txt"));
    REQUIRE_FALSE(default_result->text.contains(".cache"));

    const auto opt_in_input =
        std::string{R"({"path":")"} + dir.string() + R"(","pattern":"needle","include_hidden":true})";
    auto opt_in_result = co_await registry.dispatch(tool::kFileSearchName, opt_in_input, ctx);
    REQUIRE(opt_in_result.has_value());
    REQUIRE(opt_in_result->text.contains(".hidden.txt"));
    REQUIRE(opt_in_result->text.contains(".cache"));
  });
}

TEST_CASE("file.search reports 'no matches' (non-error) when nothing matches", "[unit][tool][file_search]") {
  TempFile file{"search-nomatch"};
  file.write("alpha\nbeta\n");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_search(registry).has_value());
    auto rules = search_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto input = std::string{R"({"path":")"} + file.string() + R"(","pattern":"absent"})";
    auto result = co_await registry.dispatch(tool::kFileSearchName, input, ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text == "no matches");
    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::allow);
  });
}

TEST_CASE("file.search returns not_found when the path does not exist", "[unit][tool][file_search]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_search(registry).has_value());
    auto rules = search_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto path = std::string{"/tmp/oran-tool-search-no-such-"} +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto input = std::string{R"({"path":")"} + path + R"(","pattern":"x"})";
    auto result = co_await registry.dispatch(tool::kFileSearchName, input, ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::not_found);
  });
}

TEST_CASE("file.search rejects malformed input as invalid_argument", "[unit][tool][file_search]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_search(registry).has_value());
    auto rules = search_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    auto bad_json = co_await registry.dispatch(tool::kFileSearchName, "{not-json}", ctx);
    REQUIRE_FALSE(bad_json.has_value());
    REQUIRE(bad_json.error().kind() == core::ErrorKind::invalid_argument);

    auto missing_path = co_await registry.dispatch(tool::kFileSearchName, R"({"pattern":"x"})", ctx);
    REQUIRE_FALSE(missing_path.has_value());
    REQUIRE(missing_path.error().kind() == core::ErrorKind::invalid_argument);

    auto missing_pattern = co_await registry.dispatch(tool::kFileSearchName, R"({"path":"/tmp/x"})", ctx);
    REQUIRE_FALSE(missing_pattern.has_value());
    REQUIRE(missing_pattern.error().kind() == core::ErrorKind::invalid_argument);

    auto wrong_path = co_await registry.dispatch(tool::kFileSearchName, R"({"path":42,"pattern":"x"})", ctx);
    REQUIRE_FALSE(wrong_path.has_value());
    REQUIRE(wrong_path.error().kind() == core::ErrorKind::invalid_argument);

    auto wrong_pattern = co_await registry.dispatch(tool::kFileSearchName, R"({"path":"/tmp/x","pattern":3})", ctx);
    REQUIRE_FALSE(wrong_pattern.has_value());
    REQUIRE(wrong_pattern.error().kind() == core::ErrorKind::invalid_argument);

    auto wrong_max_matches =
        co_await registry.dispatch(tool::kFileSearchName, R"({"path":"/tmp/x","pattern":"x","max_matches":"3"})", ctx);
    REQUIRE_FALSE(wrong_max_matches.has_value());
    REQUIRE(wrong_max_matches.error().kind() == core::ErrorKind::invalid_argument);

    auto wrong_include_hidden = co_await registry.dispatch(tool::kFileSearchName,
                                                           R"({"path":"/tmp/x","pattern":"x","include_hidden":"yes"})",
                                                           ctx);
    REQUIRE_FALSE(wrong_include_hidden.has_value());
    REQUIRE(wrong_include_hidden.error().kind() == core::ErrorKind::invalid_argument);

    auto wrong_regex =
        co_await registry.dispatch(tool::kFileSearchName, R"({"path":"/tmp/x","pattern":"x","regex":"yes"})", ctx);
    REQUIRE_FALSE(wrong_regex.has_value());
    REQUIRE(wrong_regex.error().kind() == core::ErrorKind::invalid_argument);

    auto empty_pattern = co_await registry.dispatch(tool::kFileSearchName, R"({"path":"/tmp/x","pattern":""})", ctx);
    REQUIRE_FALSE(empty_pattern.has_value());
    REQUIRE(empty_pattern.error().kind() == core::ErrorKind::invalid_argument);

    auto zero_max_matches =
        co_await registry.dispatch(tool::kFileSearchName, R"({"path":"/tmp/x","pattern":"x","max_matches":0})", ctx);
    REQUIRE_FALSE(zero_max_matches.has_value());
    REQUIRE(zero_max_matches.error().kind() == core::ErrorKind::invalid_argument);

    // Every rejected call passed the permission gate; audit recorded one allow row per attempt (10 calls).
    REQUIRE(sink.events().size() == 10);
    for (const auto& event : sink.events()) {
      REQUIRE(event.outcome == permission::AuditOutcome::allow);
    }
  });
}

// ---------------------------------------------------------------------------
// Slice 24 — `file.search` regex opt-in. Closes tech-debt #13. The default
// path stays literal substring; `"regex": true` routes the pattern through
// `permission::InputPattern` (re2 PartialMatch). Tests assert: happy regex
// on a single file, happy regex on a recursive walk, invalid regex
// surfaces as `invalid_argument` carrying the `regex_error` context entry,
// regex respects max_matches truncation, and `regex=false` matches the
// slice-20 literal-substring path verbatim (so legacy callers see no
// behavior change).

TEST_CASE("file.search regex=true matches a re2 pattern on a single file", "[unit][tool][file_search][regex]") {
  TempFile file{"search-regex-single"};
  file.write("error: 42\ninfo: ready\nerror: 7\ndebug: noop\n");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_search(registry).has_value());
    auto rules = search_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    // `\d+` matches one-or-more digits — would fail as literal substring since
    // the file contains "42" and "7" but not the literal text `\d+`.
    const auto input = std::string{R"({"path":")"} + file.string() + R"(","pattern":"error: \\d+","regex":true})";
    auto result = co_await registry.dispatch(tool::kFileSearchName, input, ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text.contains(file.string() + ":1:error: 42"));
    REQUIRE(result->text.contains(file.string() + ":3:error: 7"));
    REQUIRE_FALSE(result->text.contains("info: ready"));
    REQUIRE_FALSE(result->text.contains("debug: noop"));
    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::allow);
  });
}

TEST_CASE("file.search regex=true walks a directory and matches each line", "[unit][tool][file_search][regex]") {
  TempDir dir{"regex-recursive"};
  dir.write_file("a.txt", "TODO(alice): fix it\nfiller\n");
  dir.write_file("sub/b.txt", "filler\nTODO(bob): later\n");
  dir.write_file("sub/deep/c.txt", "TODO(carol): now\n");
  dir.write_file("noise.txt", "no marker here\n");

  test::run_async([&dir](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_search(registry).has_value());
    auto rules = search_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    // Anchored at line start (`^TODO`) plus a capture-style group — proves the
    // matcher feeds whole lines into re2 (PartialMatch with `^` collapses to
    // line-anchored under partial-match semantics). Custom raw-string
    // delimiter `X` since the JSON pattern itself contains `)"`.
    const auto input =
        std::string{R"({"path":")"} + dir.string() + R"X(","pattern":"^TODO\\([a-z]+\\)","regex":true})X";
    auto result = co_await registry.dispatch(tool::kFileSearchName, input, ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text.contains(dir.child("a.txt").string() + ":1:TODO(alice): fix it"));
    REQUIRE(result->text.contains(dir.child("sub/b.txt").string() + ":2:TODO(bob): later"));
    REQUIRE(result->text.contains(dir.child("sub/deep/c.txt").string() + ":1:TODO(carol): now"));
    REQUIRE_FALSE(result->text.contains("noise.txt"));
  });
}

TEST_CASE("file.search regex=true with invalid pattern returns invalid_argument with regex_error context",
          "[unit][tool][file_search][regex]") {
  TempFile file{"regex-bad"};
  file.write("alpha\n");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_search(registry).has_value());
    auto rules = search_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    // Unbalanced parenthesis — re2 rejects at compile time.
    const auto input = std::string{R"({"path":")"} + file.string() + R"(","pattern":"(unclosed","regex":true})";
    auto result = co_await registry.dispatch(tool::kFileSearchName, input, ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(result.error().message().contains("invalid regex"));
    REQUIRE(context_has(result.error(), "pattern", "(unclosed"));
    const auto has_regex_error_key =
        std::ranges::any_of(result.error().context(), [](const auto& entry) { return entry.first == "regex_error"; });
    REQUIRE(has_regex_error_key);
    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::allow);
  });
}

TEST_CASE("file.search regex=true honors max_matches and reports truncation", "[unit][tool][file_search][regex]") {
  TempFile file{"regex-truncate"};
  file.write("LOG 1\nLOG 2\nLOG 3\nLOG 4\nLOG 5\n");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_search(registry).has_value());
    auto rules = search_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto input =
        std::string{R"({"path":")"} + file.string() + R"(","pattern":"^LOG ","regex":true,"max_matches":2})";
    auto result = co_await registry.dispatch(tool::kFileSearchName, input, ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text.contains("(truncated; matches capped at 2)"));
    // Two match lines plus one truncation line, joined by '\n' — two internal separators.
    const auto newline_count = std::ranges::count(std::string_view{result->text}, '\n');
    REQUIRE(newline_count == 2);
  });
}

TEST_CASE("file.search regex=false explicit still treats the pattern as a literal substring",
          "[unit][tool][file_search][regex]") {
  TempFile file{"regex-false"};
  // The literal text `\d+` only appears once; if regex=false silently fell
  // through to re2 the test would also match "42" + "7".
  file.write("regex source: \\d+\n42 should not match as a digit pattern\n7 either\n");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_search(registry).has_value());
    auto rules = search_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto input = std::string{R"({"path":")"} + file.string() + R"(","pattern":"\\d+","regex":false})";
    auto result = co_await registry.dispatch(tool::kFileSearchName, input, ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text.contains(file.string() + ":1:regex source: \\d+"));
    REQUIRE_FALSE(result->text.contains(":2:"));
    REQUIRE_FALSE(result->text.contains(":3:"));
  });
}

// ---------------------------------------------------------------------------
// Slice 33 — file.search cancellation polling inside the synchronous walk.
//
// Before slice 33 the handler only checked cancellation_state before the
// executor hop and immediately after it; once `walk_and_scan` started it ran
// to completion regardless of a cancellation signal. The fix threads
// `cancellation_state` into `walk_and_scan` and `read_text_capped`, polling
// once per directory entry and once per 8 KiB read chunk. This regression
// test arms the polling by reading a multi-MiB file (forcing many chunk
// iterations inside `read_text_capped`) on a worker thread while the test
// thread emits the cancellation after a small head start. The signal lands
// during the read; the next chunk-iteration poll surfaces it as
// `core::ErrorKind::cancelled`. Driven by `orangutan-deep-review.md` §4.1.3.

TEST_CASE("file.search aborts a large-file read when cancellation fires mid-walk",
          "[unit][tool][file_search][cancellation]") {
  const auto root =
      std::filesystem::temp_directory_path() /
      ("oran-tool-search-cancel-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(root);
  // Just under the 16 MiB read cap so `read_text_capped` runs ~1024 8 KiB
  // chunks per file — plenty of polling opportunities for the cancellation
  // signal to land mid-read.
  constexpr std::size_t kPayloadBytes = 8U * 1024U * 1024U;
  const auto file_path = root / "noise.txt";
  {
    std::ofstream out{file_path, std::ios::binary};
    const std::string chunk(64U * 1024U, 'x');
    for (std::size_t written = 0; written < kPayloadBytes; written += chunk.size()) {
      out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    }
  }
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  } cleanup{root};

  asio::io_context io;
  auto work_guard = asio::make_work_guard(io);
  asio::cancellation_signal signal;
  std::optional<core::Result<tool::Output>> result;
  std::exception_ptr failure;

  tool::Registry registry;
  REQUIRE(tool::register_file_search(registry).has_value());
  auto rules = search_rule_set();
  permission::RecordingAuditSink sink;
  auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

  const auto input =
      std::string{R"({"path":")"} + file_path.string() + R"(","pattern":"this-substring-never-appears"})";
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<tool::Output>> {
        co_return co_await registry.dispatch(tool::kFileSearchName, input, ctx);
      },
      asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, core::Result<tool::Output> r) {
        failure = ep;
        result = std::move(r);
        work_guard.reset();
      }));

  std::jthread worker{[&] { io.run(); }};
  // Give the worker a head start so the coroutine is suspended deep inside
  // `read_text_capped` when the signal lands.
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  signal.emit(asio::cancellation_type::terminal);
  worker.join();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
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
  std::string identity;
  bool succeeded{false};
  std::string output_text;
  std::string error_kind;
  std::string error_message;
  std::string verdict;
};

class CaptureSink final : public orangutan::hook::Sink {
public:
  explicit CaptureSink(std::string id) : id_(std::move(id)) {}

  [[nodiscard]] std::string_view id() const noexcept override {
    return id_;
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> receive(orangutan::hook::Event event,
                                                             orangutan::hook::Payload payload) override {
    CapturedEvent row{.event = event};
    std::visit(
        [&](auto& alt) {
          using T = std::decay_t<decltype(alt)>;
          if constexpr (std::is_same_v<T, orangutan::hook::ToolBeforePayload>) {
            row.tool_name = alt.tool_name;
            row.identity = alt.who.identity;
          } else if constexpr (std::is_same_v<T, orangutan::hook::ToolDispatchedPayload>) {
            row.tool_name = alt.tool_name;
            row.identity = alt.who.identity;
            row.verdict = alt.verdict;
          } else if constexpr (std::is_same_v<T, orangutan::hook::ToolAfterPayload>) {
            row.tool_name = alt.tool_name;
            row.identity = alt.who.identity;
            row.succeeded = alt.succeeded;
            row.output_text = alt.output_text;
            row.error_kind = alt.error_kind;
            row.error_message = alt.error_message;
          } else if constexpr (std::is_same_v<T, orangutan::hook::ToolErrorPayload>) {
            row.tool_name = alt.tool_name;
            row.identity = alt.who.identity;
            row.error_kind = alt.error_kind;
            row.error_message = alt.error_message;
          }
        },
        payload);
    captures_.push_back(std::move(row));
    co_return core::Result<void>{};
  }

  [[nodiscard]] std::span<const CapturedEvent> captures() const noexcept {
    return captures_;
  }

private:
  std::string id_;
  std::vector<CapturedEvent> captures_;
};

class FailingHookSink final : public orangutan::hook::Sink {
public:
  explicit FailingHookSink(std::string id) : id_(std::move(id)) {}

  [[nodiscard]] std::string_view id() const noexcept override {
    return id_;
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> receive(orangutan::hook::Event,
                                                             orangutan::hook::Payload) override {
    co_return std::unexpected(core::Error::internal("sink rejected").with("sink", id_));
  }

private:
  std::string id_;
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
  co_return tool::Output{.text = "noop-ok"};
}

async::Awaitable<core::Result<tool::Output>> noop_error_handler(std::string_view, tool::DispatchContext& /*ctx*/) {
  co_return std::unexpected(core::Error::internal("handler exploded").with("tool", "noop"));
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

}  // namespace

TEST_CASE("dispatch publishes tool_before + tool_after on the allow path", "[unit][tool][hook]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(
        registry.add(core::ToolDef{.name = "noop", .description = "noop", .input_schema_json = "{}"}, &noop_ok_handler)
            .has_value());

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
  });
}

TEST_CASE("dispatch publishes tool_after with permission_denied kind on the deny path", "[unit][tool][hook]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(
        registry.add(core::ToolDef{.name = "noop", .description = "noop", .input_schema_json = "{}"}, &noop_ok_handler)
            .has_value());

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
    REQUIRE(
        registry
            .add(core::ToolDef{.name = "noop", .description = "noop", .input_schema_json = "{}"}, &noop_error_handler)
            .has_value());

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
    REQUIRE(
        registry.add(core::ToolDef{.name = "noop", .description = "noop", .input_schema_json = "{}"}, &noop_ok_handler)
            .has_value());

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

    // The failing sink errored on both events but the alive sink still saw them.
    REQUIRE(alive.captures().size() == 2);
  });
}

TEST_CASE("null bus reproduces slice-21 behavior — no hook publish", "[unit][tool][hook]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(
        registry.add(core::ToolDef{.name = "noop", .description = "noop", .input_schema_json = "{}"}, &noop_ok_handler)
            .has_value());

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
    REQUIRE(
        registry.add(core::ToolDef{.name = "noop", .description = "noop", .input_schema_json = "{}"}, &noop_ok_handler)
            .has_value());

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
    REQUIRE(
        registry.add(core::ToolDef{.name = "noop", .description = "noop", .input_schema_json = "{}"}, &noop_ok_handler)
            .has_value());

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
    REQUIRE(
        registry.add(core::ToolDef{.name = "noop", .description = "noop", .input_schema_json = "{}"}, &noop_ok_handler)
            .has_value());

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
    REQUIRE(
        registry.add(core::ToolDef{.name = "noop", .description = "noop", .input_schema_json = "{}"}, &noop_ok_handler)
            .has_value());

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
    REQUIRE(
        registry.add(core::ToolDef{.name = "noop", .description = "noop", .input_schema_json = "{}"}, &noop_ok_handler)
            .has_value());

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
    REQUIRE(
        registry.add(core::ToolDef{.name = "noop", .description = "noop", .input_schema_json = "{}"}, &noop_ok_handler)
            .has_value());

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
    REQUIRE(
        registry
            .add(core::ToolDef{.name = "noop", .description = "noop", .input_schema_json = "{}"}, &noop_error_handler)
            .has_value());

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
    REQUIRE(
        registry.add(core::ToolDef{.name = "noop", .description = "noop", .input_schema_json = "{}"}, &noop_ok_handler)
            .has_value());

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
    REQUIRE(
        registry.add(core::ToolDef{.name = "noop", .description = "noop", .input_schema_json = "{}"}, &noop_ok_handler)
            .has_value());

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
    REQUIRE(
        registry.add(core::ToolDef{.name = "noop", .description = "noop", .input_schema_json = "{}"}, &noop_ok_handler)
            .has_value());

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
    REQUIRE(
        registry.add(core::ToolDef{.name = "noop", .description = "noop", .input_schema_json = "{}"}, &noop_ok_handler)
            .has_value());

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
    REQUIRE(
        registry
            .add(core::ToolDef{.name = "noop", .description = "noop", .input_schema_json = "{}"}, &noop_error_handler)
            .has_value());

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

// ---------------------------------------------------------------------------
// Slice 29 — `directory.list` built-in. Wraps `oran-io::list_directory` and
// renders each entry as `<path>:<kind>:<size_bytes or '-'>`, sorted by path,
// with the literal text `no entries` for empty directories. Capability is
// the new `core::Capability::list_directory`. Tests cover: registration
// surface, happy path on a non-empty directory, empty directory rendering,
// hidden-file filtering (default vs. opt-in), max_entries error path, and
// the input-validation cases the parser must reject.

namespace {

permission::RuleSet directory_list_rule_set() {
  return single_rule(permission::Rule{
      .verdict = permission::Verdict::allow,
      .tool_pattern = std::string{tool::kDirectoryListName},
      .capability = core::Capability::list_directory,
  });
}

}  // namespace

TEST_CASE("register_directory_list advertises a `list_directory` capability and a path schema",
          "[unit][tool][directory_list]") {
  tool::Registry registry;
  REQUIRE(tool::register_directory_list(registry).has_value());
  REQUIRE(registry.size() == 1);
  const auto* def = registry.find(tool::kDirectoryListName);
  REQUIRE(def != nullptr);
  REQUIRE(def->required_capabilities.size() == 1);
  REQUIRE(def->required_capabilities[0] == core::Capability::list_directory);
  REQUIRE(def->input_schema_json.contains("\"path\""));
  REQUIRE(def->input_schema_json.contains("\"include_hidden\""));
  REQUIRE(def->input_schema_json.contains("\"max_entries\""));
}

TEST_CASE("directory.list happy path renders one entry per line, sorted by path", "[unit][tool][directory_list]") {
  TempDir dir{"list-happy"};
  dir.write_file("a.txt", "alpha");
  dir.write_file("b.txt", "be");
  // Create a subdirectory by writing a file inside it.
  dir.write_file("nested/inner.txt", "ignored at top level");

  test::run_async([&dir](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_directory_list(registry).has_value());
    auto rules = directory_list_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto input = std::string{R"({"path":")"} + dir.string() + R"("})";
    auto result = co_await registry.dispatch(tool::kDirectoryListName, input, ctx);
    REQUIRE(result.has_value());

    // Each top-level entry on its own line; `nested/inner.txt` is NOT
    // included because the listing is single-level.
    REQUIRE(result->text.contains(dir.child("a.txt").string() + ":regular_file:5"));
    REQUIRE(result->text.contains(dir.child("b.txt").string() + ":regular_file:2"));
    REQUIRE(result->text.contains(dir.child("nested").string() + ":directory:-"));
    REQUIRE_FALSE(result->text.contains("inner.txt"));

    // Two newlines join the three lines.
    const auto newline_count = std::ranges::count(std::string_view{result->text}, '\n');
    REQUIRE(newline_count == 2);

    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::allow);
  });
}

TEST_CASE("directory.list returns the literal 'no entries' for an empty directory", "[unit][tool][directory_list]") {
  TempDir dir{"list-empty"};

  test::run_async([&dir](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_directory_list(registry).has_value());
    auto rules = directory_list_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto input = std::string{R"({"path":")"} + dir.string() + R"("})";
    auto result = co_await registry.dispatch(tool::kDirectoryListName, input, ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text == "no entries");
  });
}

TEST_CASE("directory.list skips dotfiles by default and includes them with include_hidden=true",
          "[unit][tool][directory_list]") {
  TempDir dir{"list-hidden"};
  dir.write_file("visible.txt", "v");
  dir.write_file(".hidden", "h");

  test::run_async([&dir](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_directory_list(registry).has_value());
    auto rules = directory_list_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    auto default_call =
        co_await registry.dispatch(tool::kDirectoryListName, std::string{R"({"path":")"} + dir.string() + R"("})", ctx);
    REQUIRE(default_call.has_value());
    REQUIRE(default_call->text.contains("visible.txt"));
    REQUIRE_FALSE(default_call->text.contains(".hidden"));

    auto opt_in = co_await registry.dispatch(tool::kDirectoryListName,
                                             std::string{R"({"path":")"} + dir.string() + R"(","include_hidden":true})",
                                             ctx);
    REQUIRE(opt_in.has_value());
    REQUIRE(opt_in->text.contains("visible.txt"));
    REQUIRE(opt_in->text.contains(".hidden"));
  });
}

TEST_CASE("directory.list returns io when the directory exceeds max_entries", "[unit][tool][directory_list]") {
  TempDir dir{"list-overflow"};
  // Three files; ask for a cap of 2 so io::list_directory aborts at the
  // third entry with `io: directory entry limit exceeded`.
  dir.write_file("a", "1");
  dir.write_file("b", "2");
  dir.write_file("c", "3");

  test::run_async([&dir](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_directory_list(registry).has_value());
    auto rules = directory_list_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto input = std::string{R"({"path":")"} + dir.string() + R"(","max_entries":2})";
    auto result = co_await registry.dispatch(tool::kDirectoryListName, input, ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::io);
  });
}

TEST_CASE("directory.list returns not_found when the directory does not exist", "[unit][tool][directory_list]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_directory_list(registry).has_value());
    auto rules = directory_list_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto path = std::string{"/tmp/oran-tool-list-no-such-"} +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto input = std::string{R"({"path":")"} + path + R"("})";
    auto result = co_await registry.dispatch(tool::kDirectoryListName, input, ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::not_found);
  });
}

TEST_CASE("directory.list returns invalid_argument when the path is a regular file, not a directory",
          "[unit][tool][directory_list]") {
  TempFile file{"list-not-a-dir"};
  file.write("not a directory");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_directory_list(registry).has_value());
    auto rules = directory_list_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto input = std::string{R"({"path":")"} + file.string() + R"("})";
    auto result = co_await registry.dispatch(tool::kDirectoryListName, input, ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  });
}

TEST_CASE("directory.list rejects malformed input as invalid_argument", "[unit][tool][directory_list]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_directory_list(registry).has_value());
    auto rules = directory_list_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    auto bad_json = co_await registry.dispatch(tool::kDirectoryListName, "{not-json}", ctx);
    REQUIRE_FALSE(bad_json.has_value());
    REQUIRE(bad_json.error().kind() == core::ErrorKind::invalid_argument);

    auto missing_path = co_await registry.dispatch(tool::kDirectoryListName, R"({})", ctx);
    REQUIRE_FALSE(missing_path.has_value());
    REQUIRE(missing_path.error().kind() == core::ErrorKind::invalid_argument);

    auto wrong_path = co_await registry.dispatch(tool::kDirectoryListName, R"({"path":7})", ctx);
    REQUIRE_FALSE(wrong_path.has_value());
    REQUIRE(wrong_path.error().kind() == core::ErrorKind::invalid_argument);

    auto wrong_hidden =
        co_await registry.dispatch(tool::kDirectoryListName, R"({"path":"/tmp/x","include_hidden":"y"})", ctx);
    REQUIRE_FALSE(wrong_hidden.has_value());
    REQUIRE(wrong_hidden.error().kind() == core::ErrorKind::invalid_argument);

    auto wrong_max =
        co_await registry.dispatch(tool::kDirectoryListName, R"({"path":"/tmp/x","max_entries":"3"})", ctx);
    REQUIRE_FALSE(wrong_max.has_value());
    REQUIRE(wrong_max.error().kind() == core::ErrorKind::invalid_argument);

    auto zero_max = co_await registry.dispatch(tool::kDirectoryListName, R"({"path":"/tmp/x","max_entries":0})", ctx);
    REQUIRE_FALSE(zero_max.has_value());
    REQUIRE(zero_max.error().kind() == core::ErrorKind::invalid_argument);

    // All six malformed calls passed the permission gate (the parser runs
    // after the rule evaluation in the registry), so audit recorded one
    // allow row per attempt.
    REQUIRE(sink.events().size() == 6);
    for (const auto& event : sink.events()) {
      REQUIRE(event.outcome == permission::AuditOutcome::allow);
    }
  });
}

// ---------------------------------------------------------------------------
// Slice 30 — `file.delete` built-in. Thin wrapper over `oran-io::delete_file`.
// Capability is the existing `core::Capability::delete_path` (first built-in
// that actually requires it). Tests cover: registration surface, happy
// delete with the `deleted <path>` success message, missing path,
// directory refusal, symlink refusal, and the input-validation matrix.

namespace {

permission::RuleSet file_delete_rule_set() {
  return single_rule(permission::Rule{
      .verdict = permission::Verdict::allow,
      .tool_pattern = std::string{tool::kFileDeleteName},
      .capability = core::Capability::delete_path,
  });
}

}  // namespace

TEST_CASE("register_file_delete advertises a `delete_path` capability and a path schema", "[unit][tool][file_delete]") {
  tool::Registry registry;
  REQUIRE(tool::register_file_delete(registry).has_value());
  REQUIRE(registry.size() == 1);
  const auto* def = registry.find(tool::kFileDeleteName);
  REQUIRE(def != nullptr);
  REQUIRE(def->required_capabilities.size() == 1);
  REQUIRE(def->required_capabilities[0] == core::Capability::delete_path);
  REQUIRE(def->input_schema_json.contains("\"path\""));
}

TEST_CASE("file.delete happy path removes the file and reports the deletion", "[unit][tool][file_delete]") {
  TempFile file{"delete-happy"};
  file.write("doomed");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_delete(registry).has_value());
    auto rules = file_delete_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto path = file.string();
    const auto input = std::string{R"({"path":")"} + path + R"("})";
    auto result = co_await registry.dispatch(tool::kFileDeleteName, input, ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->text == "deleted " + path);
    REQUIRE_FALSE(std::filesystem::exists(path));
    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::allow);
  });
}

TEST_CASE("file.delete returns not_found when the file does not exist", "[unit][tool][file_delete]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_delete(registry).has_value());
    auto rules = file_delete_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto path = std::string{"/tmp/oran-tool-delete-no-such-"} +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto input = std::string{R"({"path":")"} + path + R"("})";
    auto result = co_await registry.dispatch(tool::kFileDeleteName, input, ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::not_found);
  });
}

TEST_CASE("file.delete refuses directories with invalid_argument and leaves them intact", "[unit][tool][file_delete]") {
  TempDir dir{"delete-dir-refused"};
  dir.write_file("guardian.txt", "still here");

  test::run_async([&dir](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_delete(registry).has_value());
    auto rules = file_delete_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto input = std::string{R"({"path":")"} + dir.string() + R"("})";
    auto result = co_await registry.dispatch(tool::kFileDeleteName, input, ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(std::filesystem::exists(dir.string()));
    REQUIRE(std::filesystem::exists(dir.child("guardian.txt")));
  });
}

TEST_CASE("file.delete refuses symlinks with invalid_argument and leaves the target intact",
          "[unit][tool][file_delete]") {
  TempDir dir{"delete-symlink-refused"};
  dir.write_file("target.txt", "untouched");
  const auto target = dir.child("target.txt");
  const auto link = dir.child("alias.txt");
  std::error_code link_ec;
  std::filesystem::create_symlink(target, link, link_ec);
  if (link_ec) {
    SUCCEED("symlink creation not supported on this filesystem: " << link_ec.message());
    return;
  }

  test::run_async([&dir, &link, &target](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_delete(registry).has_value());
    auto rules = file_delete_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto input = std::string{R"({"path":")"} + link.string() + R"("})";
    auto result = co_await registry.dispatch(tool::kFileDeleteName, input, ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(std::filesystem::is_symlink(link));
    REQUIRE(std::filesystem::exists(target));
    (void)dir;
  });
}

TEST_CASE("file.delete rejects malformed input as invalid_argument", "[unit][tool][file_delete]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_delete(registry).has_value());
    auto rules = file_delete_rule_set();
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    auto bad_json = co_await registry.dispatch(tool::kFileDeleteName, "{not-json}", ctx);
    REQUIRE_FALSE(bad_json.has_value());
    REQUIRE(bad_json.error().kind() == core::ErrorKind::invalid_argument);

    auto missing_path = co_await registry.dispatch(tool::kFileDeleteName, R"({})", ctx);
    REQUIRE_FALSE(missing_path.has_value());
    REQUIRE(missing_path.error().kind() == core::ErrorKind::invalid_argument);

    auto wrong_path = co_await registry.dispatch(tool::kFileDeleteName, R"({"path":7})", ctx);
    REQUIRE_FALSE(wrong_path.has_value());
    REQUIRE(wrong_path.error().kind() == core::ErrorKind::invalid_argument);

    // All three malformed calls passed the permission gate; audit recorded
    // one allow row per attempt.
    REQUIRE(sink.events().size() == 3);
    for (const auto& event : sink.events()) {
      REQUIRE(event.outcome == permission::AuditOutcome::allow);
    }
  });
}

TEST_CASE("file.delete deny verdict short-circuits and does not unlink the file", "[unit][tool][file_delete]") {
  TempFile file{"delete-denied"};
  file.write("survives");

  test::run_async([&file](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_delete(registry).has_value());
    permission::RuleSet rules;
    rules.add(permission::Rule{
        .verdict = permission::Verdict::deny,
        .tool_pattern = std::string{tool::kFileDeleteName},
        .capability = core::Capability::delete_path,
    });
    permission::RecordingAuditSink sink;
    auto ctx = make_ctx(io, rules, sink, permission::Mode::strict);

    const auto input = std::string{R"({"path":")"} + file.string() + R"("})";
    auto result = co_await registry.dispatch(tool::kFileDeleteName, input, ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(std::filesystem::exists(file.string()));
    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::deny);
  });
}
