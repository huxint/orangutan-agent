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
#include <utility>

#include <asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <oran/async.hpp>
#include <oran/core/capability.hpp>
#include <oran/core/error.hpp>
#include <oran/core/tool_def.hpp>
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
  REQUIRE(catalog.size() == 4);
  REQUIRE(catalog[0].name == tool::kFileReadName);
  REQUIRE(catalog[1].name == tool::kFileWriteName);
  REQUIRE(catalog[2].name == tool::kFileEditName);
  REQUIRE(catalog[3].name == tool::kFileSearchName);
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

    auto empty_pattern = co_await registry.dispatch(tool::kFileSearchName, R"({"path":"/tmp/x","pattern":""})", ctx);
    REQUIRE_FALSE(empty_pattern.has_value());
    REQUIRE(empty_pattern.error().kind() == core::ErrorKind::invalid_argument);

    auto zero_max_matches =
        co_await registry.dispatch(tool::kFileSearchName, R"({"path":"/tmp/x","pattern":"x","max_matches":0})", ctx);
    REQUIRE_FALSE(zero_max_matches.has_value());
    REQUIRE(zero_max_matches.error().kind() == core::ErrorKind::invalid_argument);

    // Every rejected call passed the permission gate; audit recorded one allow row per attempt (9 calls).
    REQUIRE(sink.events().size() == 9);
    for (const auto& event : sink.events()) {
      REQUIRE(event.outcome == permission::AuditOutcome::allow);
    }
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
