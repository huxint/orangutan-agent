// tests/tool/test_workspace.cpp — workspace resolver coverage.

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <oran/async.hpp>
#include <oran/core/error.hpp>
#include <oran/core/time.hpp>
#include <oran/permission.hpp>
#include <oran/tool.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace permission = orangutan::permission;
namespace tool = orangutan::tool;
namespace test = orangutan::tests;

namespace {

class TempDir {
public:
  explicit TempDir(std::string name)
      : path_{std::filesystem::temp_directory_path() /
              (std::move(name) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))} {
    std::filesystem::create_directories(path_);
  }

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

void write_text(const std::filesystem::path& path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out{path, std::ios::binary};
  REQUIRE(out.good());
  out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

[[nodiscard]] bool context_has(const core::Error& error, std::string_view key, std::string_view value) {
  return std::ranges::any_of(error.context(),
                             [&](const auto& entry) { return entry.first == key && entry.second == value; });
}

[[nodiscard]] bool is_hex_digest(std::string_view value) {
  return value.size() == 64U &&
         std::ranges::all_of(value, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

[[nodiscard]] nlohmann::json path_resolution_metadata(const permission::AuditEvent& event) {
  auto metadata = nlohmann::json::parse(event.metadata_json);
  REQUIRE(metadata.is_object());
  REQUIRE(metadata.contains("path_resolution"));
  auto path_resolution = metadata["path_resolution"];
  REQUIRE(path_resolution.is_object());
  return path_resolution;
}

void create_symlink_or_skip(const std::filesystem::path& target, const std::filesystem::path& link) {
  std::error_code ec;
  std::filesystem::create_symlink(target, link, ec);
  if (ec) {
    SKIP(std::string{"test filesystem does not allow symlink creation: "} + ec.message());
  }
}

[[nodiscard]] tool::Workspace make_workspace(const std::filesystem::path& root, tool::WorkspaceOptions options = {}) {
  auto workspace = tool::Workspace::create(root.string(), std::move(options));
  REQUIRE(workspace.has_value());
  return std::move(*workspace);
}

[[nodiscard]] permission::RuleSet allow_file_read_rules() {
  permission::RuleSet rules;
  rules.add(permission::Rule{.verdict = permission::Verdict::allow, .tool_pattern = "FileRead"});
  return rules;
}

[[nodiscard]] permission::RuleSet allow_tool_rules(std::string tool_name, core::Capability capability) {
  permission::RuleSet rules;
  rules.add(permission::Rule{
      .verdict = permission::Verdict::allow,
      .tool_pattern = std::move(tool_name),
      .capability = capability,
  });
  return rules;
}

[[nodiscard]] permission::RuleSet ask_tool_rules(std::string tool_name, core::Capability capability) {
  permission::RuleSet rules;
  rules.add(permission::Rule{
      .verdict = permission::Verdict::ask,
      .tool_pattern = std::move(tool_name),
      .capability = capability,
      .replay_max = 1,
      .approval_ttl = std::chrono::seconds{60},
  });
  return rules;
}

[[nodiscard]] tool::DispatchContext make_workspace_ctx(asio::io_context& io,
                                                       permission::RuleSet& rules,
                                                       permission::AuditSink& sink,
                                                       tool::Workspace& workspace) {
  return tool::DispatchContext{
      .executor = io.get_executor(),
      .mode = permission::Mode::default_,
      .rules = rules,
      .audit = sink,
      .workspace = &workspace,
      .scope_key = "scope-A",
      .agent_key = "coder",
      .identity = "operator-1",
  };
}

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
                                              core::Time now) {
  return broker.approve(
      permission::ApprovalGrant{
          .tool_name = tool_name,
          .input = input,
          .identity = identity,
          .ttl = std::chrono::seconds{60},
          .replay_max = 1,
      },
      now);
}

}  // namespace

TEST_CASE("Workspace resolves relative and absolute paths under the root", "[unit][tool][workspace]") {
  TempDir root{"oran-workspace-inside"};
  write_text(root.path() / "nested" / "note.txt", "hello");
  auto workspace = make_workspace(root.path());

  auto relative = workspace.resolve_read("nested/note.txt");
  REQUIRE(relative.has_value());
  REQUIRE(relative->absolute_path == (root.path() / "nested" / "note.txt").string());
  REQUIRE(relative->relative_path == "nested/note.txt");
  REQUIRE_FALSE(relative->symlink_followed);
  REQUIRE_FALSE(relative->outside_workspace_explicit_override);

  auto absolute = workspace.resolve_read((root.path() / "nested" / "note.txt").string());
  REQUIRE(absolute.has_value());
  REQUIRE(absolute->absolute_path == relative->absolute_path);
  REQUIRE(absolute->relative_path == relative->relative_path);
}

TEST_CASE("Workspace rejects traversal outside the root", "[unit][tool][workspace]") {
  TempDir root{"oran-workspace-traversal"};
  auto workspace = make_workspace(root.path());

  auto read = workspace.resolve_read("../outside.txt");
  REQUIRE_FALSE(read.has_value());
  REQUIRE(read.error().kind() == core::ErrorKind::permission_denied);
  REQUIRE(context_has(read.error(), "reason", "outside_workspace"));

  const auto deny_cases = {
      std::string{"../outside.txt"},
      std::string{"/etc/passwd"},
      std::string{"legit/../../../outside.txt"},
  };
  for (const auto& input : deny_cases) {
    auto resolved = workspace.resolve_write(input, tool::WriteIntent{});
    REQUIRE_FALSE(resolved.has_value());
    REQUIRE(resolved.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(context_has(resolved.error(), "reason", "outside_workspace"));
  }
}

TEST_CASE("Workspace follows inside read symlinks and rejects symlink escapes", "[unit][tool][workspace]") {
  TempDir root{"oran-workspace-symlink-root"};
  TempDir outside{"oran-workspace-symlink-outside"};
  write_text(root.path() / "target.txt", "inside");
  write_text(outside.path() / "secret.txt", "outside");

  create_symlink_or_skip(root.path() / "target.txt", root.path() / "inside-link.txt");
  create_symlink_or_skip(outside.path() / "secret.txt", root.path() / "outside-link.txt");

  auto workspace = make_workspace(root.path());

  auto inside = workspace.resolve_read("inside-link.txt");
  REQUIRE(inside.has_value());
  REQUIRE(inside->absolute_path == (root.path() / "target.txt").string());
  REQUIRE(inside->symlink_followed);

  auto escaped = workspace.resolve_read("outside-link.txt");
  REQUIRE_FALSE(escaped.has_value());
  REQUIRE(escaped.error().kind() == core::ErrorKind::permission_denied);
  REQUIRE(context_has(escaped.error(), "reason", "symlink_escape"));
}

TEST_CASE("Workspace refuses mutating paths that traverse symlinks", "[unit][tool][workspace]") {
  TempDir root{"oran-workspace-write-symlink"};
  write_text(root.path() / "target.txt", "inside");
  create_symlink_or_skip(root.path() / "target.txt", root.path() / "link.txt");
  auto workspace = make_workspace(root.path());

  auto write = workspace.resolve_write("link.txt", tool::WriteIntent{});
  REQUIRE_FALSE(write.has_value());
  REQUIRE(write.error().kind() == core::ErrorKind::permission_denied);
  REQUIRE(context_has(write.error(), "reason", "symlink_target"));

  auto deleted = workspace.resolve_delete("link.txt");
  REQUIRE_FALSE(deleted.has_value());
  REQUIRE(deleted.error().kind() == core::ErrorKind::permission_denied);
  REQUIRE(context_has(deleted.error(), "reason", "symlink_target"));
}

TEST_CASE("Workspace extra roots widen only the configured direction", "[unit][tool][workspace]") {
  TempDir root{"oran-workspace-primary"};
  TempDir readable{"oran-workspace-readable"};
  TempDir writable{"oran-workspace-writable"};
  write_text(readable.path() / "audit.log", "audit");

  auto workspace = make_workspace(root.path(),
                                  tool::WorkspaceOptions{
                                      .extra_read_roots = {readable.path().string()},
                                      .extra_write_roots = {writable.path().string()},
                                  });

  auto read = workspace.resolve_read((readable.path() / "audit.log").string());
  REQUIRE(read.has_value());
  REQUIRE(read->outside_workspace_explicit_override);
  REQUIRE(read->override_root_index.has_value());
  REQUIRE(*read->override_root_index == 0U);
  REQUIRE(read->relative_path == "audit.log");

  auto write_to_read_root = workspace.resolve_write((readable.path() / "audit.log").string(), tool::WriteIntent{});
  REQUIRE_FALSE(write_to_read_root.has_value());
  REQUIRE(write_to_read_root.error().kind() == core::ErrorKind::permission_denied);
  REQUIRE(context_has(write_to_read_root.error(), "reason", "outside_workspace"));

  auto write_to_write_root = workspace.resolve_write((writable.path() / "created.txt").string(), tool::WriteIntent{});
  REQUIRE(write_to_write_root.has_value());
  REQUIRE(write_to_write_root->outside_workspace_explicit_override);
  REQUIRE(write_to_write_root->override_root_index.has_value());
  REQUIRE(*write_to_write_root->override_root_index == 0U);
}

TEST_CASE("Workspace per-call outside read/list override resolves existing paths only for read-side intents",
          "[unit][tool][workspace]") {
  TempDir root{"oran-workspace-per-call-primary"};
  TempDir outside{"oran-workspace-per-call-outside"};
  write_text(outside.path() / "audit.log", "audit");

  auto workspace = make_workspace(root.path());

  auto read = workspace.resolve_read_outside_workspace((outside.path() / "audit.log").string());
  REQUIRE(read.has_value());
  REQUIRE(read->absolute_path == (outside.path() / "audit.log").string());
  REQUIRE(read->relative_path.empty());
  REQUIRE(read->outside_workspace_explicit_override);
  REQUIRE(read->per_call_outside_workspace_override);
  REQUIRE_FALSE(read->override_root_index.has_value());

  auto list = workspace.resolve_list_outside_workspace(outside.path().string());
  REQUIRE(list.has_value());
  REQUIRE(list->absolute_path == outside.path().string());
  REQUIRE(list->outside_workspace_explicit_override);
  REQUIRE(list->per_call_outside_workspace_override);

  auto write = workspace.resolve_write((outside.path() / "created.txt").string(), tool::WriteIntent{});
  REQUIRE_FALSE(write.has_value());
  REQUIRE(write.error().kind() == core::ErrorKind::permission_denied);
  REQUIRE(context_has(write.error(), "reason", "outside_workspace"));
}

TEST_CASE("Workspace walk filter shares hidden, built-in, and ignore-file decisions", "[unit][tool][workspace]") {
  TempDir root{"oran-workspace-walk-filter"};
  write_text(root.path() / ".gitignore", "*.log\nignored/\ndocs/secret.txt\n!keep.log\n");
  write_text(root.path() / "src" / ".ignore", "local.txt\n");
  write_text(root.path() / "src" / "local.txt", "ignored by nested rule");
  write_text(root.path() / "docs" / "secret.txt", "ignored by slash rule");

  auto workspace = make_workspace(root.path());
  auto filter = workspace.walk_filter(root.path().string());

  REQUIRE(filter.should_skip((root.path() / ".hidden.txt").string(), false));
  REQUIRE(filter.should_skip((root.path() / ".git").string(), true));
  REQUIRE(filter.should_skip((root.path() / "build").string(), true));
  REQUIRE(filter.should_skip((root.path() / "a.log").string(), false));
  REQUIRE_FALSE(filter.should_skip((root.path() / "keep.log").string(), false));
  REQUIRE(filter.should_skip((root.path() / "ignored").string(), true));
  REQUIRE(filter.should_skip((root.path() / "src" / "local.txt").string(), false));
  REQUIRE(filter.should_skip((root.path() / "docs" / "secret.txt").string(), false));

  auto forensic_filter = workspace.walk_filter(root.path().string(),
                                               tool::WorkspaceWalkOptions{
                                                   .include_hidden = true,
                                                   .respect_ignore = false,
                                               });
  REQUIRE_FALSE(forensic_filter.should_skip((root.path() / ".hidden.txt").string(), false));
  REQUIRE_FALSE(forensic_filter.should_skip((root.path() / "build").string(), true));
  REQUIRE_FALSE(forensic_filter.should_skip((root.path() / "a.log").string(), false));
}

TEST_CASE("Workspace display_path renders stable root-relative labels", "[unit][tool][workspace]") {
  TempDir root{"oran-workspace-display"};
  TempDir readable{"oran-workspace-display-readable"};
  write_text(root.path() / "src" / "main.cpp", "int main() {}\n");
  write_text(readable.path() / "logs" / "audit.txt", "audit\n");
  auto workspace = make_workspace(root.path(),
                                  tool::WorkspaceOptions{
                                      .extra_read_roots = {readable.path().string()},
                                  });

  REQUIRE(workspace.display_path(root.path().string()) == "<workspace>");
  REQUIRE(workspace.display_path((root.path() / "src" / "main.cpp").string()) == "<workspace>/src/main.cpp");
  REQUIRE(workspace.display_path((readable.path() / "logs" / "audit.txt").string()) == "<read-root-0>/logs/audit.txt");
  REQUIRE(workspace.display_path("/tmp/oran-outside-display.txt") == "/tmp/oran-outside-display.txt");
}

TEST_CASE("Workspace instances keep independent roots", "[unit][tool][workspace]") {
  TempDir left{"oran-workspace-left"};
  TempDir right{"oran-workspace-right"};
  write_text(left.path() / "foo.txt", "left");
  write_text(right.path() / "foo.txt", "right");

  auto left_workspace = make_workspace(left.path());
  auto right_workspace = make_workspace(right.path());

  auto left_resolved = left_workspace.resolve_read("foo.txt");
  auto right_resolved = right_workspace.resolve_read("foo.txt");
  REQUIRE(left_resolved.has_value());
  REQUIRE(right_resolved.has_value());
  REQUIRE(left_resolved->absolute_path == (left.path() / "foo.txt").string());
  REQUIRE(right_resolved->absolute_path == (right.path() / "foo.txt").string());
  REQUIRE(left_resolved->absolute_path != right_resolved->absolute_path);
}

TEST_CASE("Registry pre-resolves workspace paths before permission evaluation and records audit metadata",
          "[unit][tool][workspace][audit]") {
  TempDir root{"oran-workspace-audit-deny"};
  write_text(root.path() / "note.txt", "inside");

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_read(registry).has_value());

    permission::RuleSet rules;
    rules.add(permission::Rule{
        .verdict = permission::Verdict::deny,
        .tool_pattern = std::string{tool::kFileReadName},
        .capability = core::Capability::read_file,
    });
    permission::RecordingAuditSink sink;
    auto workspace = make_workspace(root.path());
    auto ctx = make_workspace_ctx(io, rules, sink, workspace);

    auto denied = co_await registry.dispatch(tool::kFileReadName, R"({"path":"note.txt"})", ctx);
    REQUIRE_FALSE(denied.has_value());
    REQUIRE(denied.error().kind() == core::ErrorKind::permission_denied);

    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::deny);
    const auto metadata = path_resolution_metadata(sink.events()[0]);
    REQUIRE(metadata["resolved_relative_path"] == "note.txt");
    REQUIRE(metadata["input_path_hash"] == permission::to_hex(permission::ApprovalAuthority::input_hash("note.txt")));
    REQUIRE(metadata["workspace_root_hash"] ==
            permission::to_hex(permission::ApprovalAuthority::input_hash(workspace.root())));
    REQUIRE(metadata["symlink_followed"] == false);
    REQUIRE(metadata["created_parents"] == false);
    REQUIRE(metadata["outside_workspace_explicit_override"] == false);
    REQUIRE(metadata["override_root_index"].is_null());
  });
}

TEST_CASE("Registry audit metadata records workspace override root matches", "[unit][tool][workspace][audit]") {
  TempDir root{"oran-workspace-audit-primary"};
  TempDir readable{"oran-workspace-audit-readable"};
  write_text(readable.path() / "audit.log", "needle in readable root");

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_read(registry).has_value());

    auto workspace = make_workspace(root.path(),
                                    tool::WorkspaceOptions{
                                        .extra_read_roots = {readable.path().string()},
                                    });
    auto rules = allow_file_read_rules();
    permission::RecordingAuditSink sink;
    auto ctx = make_workspace_ctx(io, rules, sink, workspace);

    const auto input = std::format(R"({{"path":"{}"}})", (readable.path() / "audit.log").string());
    auto read = co_await registry.dispatch(tool::kFileReadName, input, ctx);
    REQUIRE(read.has_value());
    REQUIRE(read->text.contains("needle in readable root"));

    REQUIRE(sink.events().size() == 1);
    const auto metadata = path_resolution_metadata(sink.events()[0]);
    REQUIRE(metadata["resolved_relative_path"] == "audit.log");
    REQUIRE(metadata["outside_workspace_explicit_override"] == true);
    REQUIRE(metadata["override_root_index"] == 0);
    REQUIRE(is_hex_digest(metadata["input_path_hash"].get<std::string>()));
    REQUIRE(is_hex_digest(metadata["workspace_root_hash"].get<std::string>()));
  });
}

TEST_CASE("Registry audits path policy failures before ask approval and does not spend replay",
          "[unit][tool][workspace][audit][approval]") {
  TempDir root{"oran-workspace-audit-fail"};
  TempDir outside{"oran-workspace-audit-fail-outside"};
  write_text(outside.path() / "secret.txt", "outside");

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_read(registry).has_value());

    auto workspace = make_workspace(root.path(),
                                    tool::WorkspaceOptions{
                                        .extra_write_roots = {outside.path().string()},
                                    });
    auto rules = ask_tool_rules(std::string{tool::kFileReadName}, core::Capability::read_file);
    permission::RecordingAuditSink sink;
    auto ctx = make_workspace_ctx(io, rules, sink, workspace);

    std::error_code ec;
    const auto outside_relative_path = std::filesystem::relative(outside.path() / "secret.txt", root.path(), ec);
    REQUIRE(ec.value() == 0);
    const auto input = std::format(R"({{"path":"{}"}})", outside_relative_path.string());

    auto broker = make_broker();
    const auto now = fixed_now();
    const auto token = grant(broker, tool::kFileReadName, input, "operator-1", now);
    ctx.approval_broker = &broker;
    ctx.approval_token = &token;
    ctx.now = now;

    auto denied = co_await registry.dispatch(tool::kFileReadName, input, ctx);
    REQUIRE_FALSE(denied.has_value());
    REQUIRE(denied.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(context_has(denied.error(), "reason", "outside_workspace"));

    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].verdict == permission::Verdict::ask);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::ask);
    const auto metadata = path_resolution_metadata(sink.events()[0]);
    REQUIRE(metadata["resolved_relative_path"].is_null());
    REQUIRE(metadata["error_kind"] == "permission_denied");
    REQUIRE(metadata["error_reason"] == "outside_workspace");
    REQUIRE(is_hex_digest(metadata["input_path_hash"].get<std::string>()));
    REQUIRE(is_hex_digest(metadata["workspace_root_hash"].get<std::string>()));

    auto still_unspent = broker.check(token, tool::kFileReadName, input, "operator-1", now);
    REQUIRE(still_unspent.has_value());
    auto now_spent = broker.check(token, tool::kFileReadName, input, "operator-1", now);
    REQUIRE_FALSE(now_spent.has_value());
    REQUIRE(context_has(now_spent.error(), "reason", "replay_exhausted"));
  });
}

TEST_CASE("Read-side outside-workspace override forces approval and records explicit audit display",
          "[unit][tool][workspace][audit][approval]") {
  TempDir root{"oran-workspace-readside-override-primary"};
  TempDir outside{"oran-workspace-readside-override-outside"};
  write_text(outside.path() / "secret.txt", "outside");

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_read(registry).has_value());

    auto workspace = make_workspace(root.path(),
                                    tool::WorkspaceOptions{
                                        .extra_write_roots = {outside.path().string()},
                                    });
    auto rules = allow_file_read_rules();
    permission::RecordingAuditSink sink;
    auto ctx = make_workspace_ctx(io, rules, sink, workspace);

    const auto input =
        std::format(R"({{"path":"{}","allow_outside_workspace":true}})", (outside.path() / "secret.txt").string());
    auto denied = co_await registry.dispatch(tool::kFileReadName, input, ctx);
    REQUIRE_FALSE(denied.has_value());
    REQUIRE(denied.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(context_has(denied.error(), "reason", "approval_required"));
    REQUIRE(context_has(denied.error(), "decision_reason", "outside_workspace_override"));

    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].verdict == permission::Verdict::ask);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::ask);
    REQUIRE(sink.events()[0].reason == "outside_workspace_override");
    const auto metadata = path_resolution_metadata(sink.events()[0]);
    REQUIRE(metadata["resolved_relative_path"].is_null());
    REQUIRE(metadata["resolved_display_path"] == (outside.path() / "secret.txt").string());
    REQUIRE(metadata["outside_workspace_explicit_override"] == true);
    REQUIRE(metadata["per_call_outside_workspace_override"] == true);
    REQUIRE(metadata["override_root_index"].is_null());
  });
}

TEST_CASE("Approved read-side outside-workspace override runs read search and list tools",
          "[unit][tool][workspace][audit][approval]") {
  TempDir root{"oran-workspace-readside-approved-primary"};
  TempDir outside{"oran-workspace-readside-approved-outside"};
  write_text(outside.path() / "secret.txt", "outside needle");
  write_text(outside.path() / "tree" / "a.txt", "a");

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_read(registry).has_value());
    REQUIRE(tool::register_file_search(registry).has_value());
    REQUIRE(tool::register_directory_list(registry).has_value());

    permission::RuleSet rules;
    rules.add(permission::Rule{
        .verdict = permission::Verdict::allow,
        .tool_pattern = std::string{tool::kFileReadName},
        .capability = core::Capability::read_file,
    });
    rules.add(permission::Rule{
        .verdict = permission::Verdict::allow,
        .tool_pattern = std::string{tool::kFileSearchName},
        .capability = core::Capability::read_file,
    });
    rules.add(permission::Rule{
        .verdict = permission::Verdict::allow,
        .tool_pattern = std::string{tool::kDirectoryListName},
        .capability = core::Capability::list_directory,
    });

    auto workspace = make_workspace(root.path());
    permission::RecordingAuditSink sink;
    auto ctx = make_workspace_ctx(io, rules, sink, workspace);
    auto broker = make_broker();
    const auto now = fixed_now();
    ctx.approval_broker = &broker;
    ctx.now = now;

    const auto read_input =
        std::format(R"({{"path":"{}","allow_outside_workspace":true}})", (outside.path() / "secret.txt").string());
    auto read_token = grant(broker, tool::kFileReadName, read_input, "operator-1", now);
    ctx.approval_token = &read_token;
    auto read = co_await registry.dispatch(tool::kFileReadName, read_input, ctx);
    REQUIRE(read.has_value());
    REQUIRE(read->text.contains("outside needle"));

    const auto search_input = std::format(R"({{"path":"{}","pattern":"needle","allow_outside_workspace":true}})",
                                          (outside.path() / "secret.txt").string());
    auto search_token = grant(broker, tool::kFileSearchName, search_input, "operator-1", now);
    ctx.approval_token = &search_token;
    auto searched = co_await registry.dispatch(tool::kFileSearchName, search_input, ctx);
    REQUIRE(searched.has_value());
    REQUIRE(searched->text.contains("outside needle"));

    const auto list_input =
        std::format(R"({{"path":"{}","allow_outside_workspace":true}})", (outside.path() / "tree").string());
    auto list_token = grant(broker, tool::kDirectoryListName, list_input, "operator-1", now);
    ctx.approval_token = &list_token;
    auto listed = co_await registry.dispatch(tool::kDirectoryListName, list_input, ctx);
    REQUIRE(listed.has_value());
    REQUIRE(listed->text.contains((outside.path() / "tree" / "a.txt").string()));

    REQUIRE(sink.events().size() == 3);
    for (const auto& event : sink.events()) {
      REQUIRE(event.verdict == permission::Verdict::ask);
      REQUIRE(event.outcome == permission::AuditOutcome::approved);
      REQUIRE(event.reason == "outside_workspace_override");
      const auto metadata = path_resolution_metadata(event);
      REQUIRE(metadata["resolved_relative_path"].is_null());
      REQUIRE(metadata["outside_workspace_explicit_override"] == true);
      REQUIRE(metadata["per_call_outside_workspace_override"] == true);
      REQUIRE(metadata["override_root_index"].is_null());
    }
    REQUIRE(path_resolution_metadata(sink.events()[0])["resolved_display_path"] ==
            (outside.path() / "secret.txt").string());
    REQUIRE(path_resolution_metadata(sink.events()[1])["resolved_display_path"] ==
            (outside.path() / "secret.txt").string());
    REQUIRE(path_resolution_metadata(sink.events()[2])["resolved_display_path"] == (outside.path() / "tree").string());
  });
}

TEST_CASE("Registry leaves malformed write options to the handler instead of pre-resolving the path",
          "[unit][tool][workspace][audit]") {
  TempDir root{"oran-workspace-malformed-write"};
  TempDir outside{"oran-workspace-malformed-write-outside"};

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_write(registry).has_value());

    auto workspace = make_workspace(root.path());
    auto rules = allow_tool_rules(std::string{tool::kFileWriteName}, core::Capability::write_file);
    permission::RecordingAuditSink sink;
    auto ctx = make_workspace_ctx(io, rules, sink, workspace);

    std::error_code ec;
    const auto outside_relative_path = std::filesystem::relative(outside.path() / "blocked.txt", root.path(), ec);
    REQUIRE(ec.value() == 0);
    const auto input =
        std::format(R"({{"path":"{}","content":"escape","create_parents":"yes"}})", outside_relative_path.string());

    auto rejected = co_await registry.dispatch(tool::kFileWriteName, input, ctx);
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE_FALSE(context_has(rejected.error(), "reason", "outside_workspace"));
    REQUIRE_FALSE(std::filesystem::exists(outside.path() / "blocked.txt"));

    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].metadata_json == "{}");
  });
}

TEST_CASE("FileRead uses DispatchContext workspace when supplied", "[unit][tool][workspace][file_read]") {
  TempDir root{"oran-workspace-file-read"};
  TempDir outside{"oran-workspace-file-read-outside"};
  write_text(root.path() / "note.txt", "inside");
  write_text(outside.path() / "secret.txt", "outside");

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_read(registry).has_value());

    auto workspace = make_workspace(root.path());
    auto rules = allow_file_read_rules();
    permission::RecordingAuditSink sink;
    auto ctx = make_workspace_ctx(io, rules, sink, workspace);

    auto read = co_await registry.dispatch("FileRead", R"({"path":"note.txt"})", ctx);
    REQUIRE(read.has_value());
    REQUIRE(read->text.contains("\ninside"));
    REQUIRE(read->text.contains("fingerprint=v1:"));

    std::error_code ec;
    const auto outside_relative_path = std::filesystem::relative(outside.path() / "secret.txt", root.path(), ec);
    REQUIRE(ec.value() == 0);
    const auto outside_relative = outside_relative_path.string();
    const auto escaped_input = std::format(R"({{"path":"{}"}})", outside_relative);
    auto escaped = co_await registry.dispatch("FileRead", escaped_input, ctx);
    REQUIRE_FALSE(escaped.has_value());
    REQUIRE(escaped.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(context_has(escaped.error(), "reason", "outside_workspace"));
  });
}

TEST_CASE("FileWrite uses DispatchContext workspace for relative writes and traversal refusal",
          "[unit][tool][workspace][file_write]") {
  TempDir root{"oran-workspace-file-write"};
  TempDir outside{"oran-workspace-file-write-outside"};

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_write(registry).has_value());

    auto workspace = make_workspace(root.path());
    auto rules = allow_tool_rules(std::string{tool::kFileWriteName}, core::Capability::write_file);
    permission::RecordingAuditSink sink;
    auto ctx = make_workspace_ctx(io, rules, sink, workspace);

    auto written = co_await registry.dispatch(tool::kFileWriteName,
                                              R"({"path":"nested/out.txt","content":"inside","create_parents":true})",
                                              ctx);
    REQUIRE(written.has_value());
    REQUIRE(std::filesystem::exists(root.path() / "nested" / "out.txt"));

    std::error_code ec;
    const auto outside_relative_path = std::filesystem::relative(outside.path() / "blocked.txt", root.path(), ec);
    REQUIRE(ec.value() == 0);
    const auto escaped_input =
        std::format(R"({{"path":"{}","content":"escape","create_parents":true}})", outside_relative_path.string());
    auto escaped = co_await registry.dispatch(tool::kFileWriteName, escaped_input, ctx);
    REQUIRE_FALSE(escaped.has_value());
    REQUIRE(escaped.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(context_has(escaped.error(), "reason", "outside_workspace"));
    REQUIRE_FALSE(std::filesystem::exists(outside.path() / "blocked.txt"));
  });

  std::ifstream written{root.path() / "nested" / "out.txt", std::ios::binary};
  REQUIRE(std::string{std::istreambuf_iterator<char>{written}, std::istreambuf_iterator<char>{}} == "inside");
}

TEST_CASE("FileEdit uses DispatchContext workspace for relative edits and traversal refusal",
          "[unit][tool][workspace][file_edit]") {
  TempDir root{"oran-workspace-file-edit"};
  TempDir outside{"oran-workspace-file-edit-outside"};
  write_text(root.path() / "note.txt", "alpha beta");
  write_text(outside.path() / "secret.txt", "do not edit");

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_edit(registry).has_value());

    auto workspace = make_workspace(root.path());
    auto rules = allow_tool_rules(std::string{tool::kFileEditName}, core::Capability::edit_file);
    permission::RecordingAuditSink sink;
    auto ctx = make_workspace_ctx(io, rules, sink, workspace);

    auto edited = co_await registry.dispatch(tool::kFileEditName,
                                             R"({"path":"note.txt","old_string":"beta","new_string":"BETA"})",
                                             ctx);
    REQUIRE(edited.has_value());

    std::error_code ec;
    const auto outside_relative_path = std::filesystem::relative(outside.path() / "secret.txt", root.path(), ec);
    REQUIRE(ec.value() == 0);
    const auto escaped_input =
        std::format(R"({{"path":"{}","old_string":"do","new_string":"DO"}})", outside_relative_path.string());
    auto escaped = co_await registry.dispatch(tool::kFileEditName, escaped_input, ctx);
    REQUIRE_FALSE(escaped.has_value());
    REQUIRE(escaped.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(context_has(escaped.error(), "reason", "outside_workspace"));
  });

  std::ifstream inside{root.path() / "note.txt", std::ios::binary};
  REQUIRE(std::string{std::istreambuf_iterator<char>{inside}, std::istreambuf_iterator<char>{}} == "alpha BETA");
  std::ifstream outside_file{outside.path() / "secret.txt", std::ios::binary};
  REQUIRE(std::string{std::istreambuf_iterator<char>{outside_file}, std::istreambuf_iterator<char>{}} == "do not edit");
}

TEST_CASE("FileEdit rejects workspace symlink mutation targets", "[unit][tool][workspace][file_edit]") {
  TempDir root{"oran-workspace-file-edit-link"};
  write_text(root.path() / "target.txt", "alpha");
  create_symlink_or_skip(root.path() / "target.txt", root.path() / "link.txt");

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_edit(registry).has_value());

    auto workspace = make_workspace(root.path());
    auto rules = allow_tool_rules(std::string{tool::kFileEditName}, core::Capability::edit_file);
    permission::RecordingAuditSink sink;
    auto ctx = make_workspace_ctx(io, rules, sink, workspace);

    auto edited = co_await registry.dispatch(tool::kFileEditName,
                                             R"({"path":"link.txt","old_string":"alpha","new_string":"ALPHA"})",
                                             ctx);
    REQUIRE_FALSE(edited.has_value());
    REQUIRE(edited.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(context_has(edited.error(), "reason", "symlink_target"));
    REQUIRE(std::filesystem::is_symlink(root.path() / "link.txt"));
  });

  std::ifstream target{root.path() / "target.txt", std::ios::binary};
  REQUIRE(std::string{std::istreambuf_iterator<char>{target}, std::istreambuf_iterator<char>{}} == "alpha");
}

TEST_CASE("FileDelete uses DispatchContext workspace for relative deletes and traversal refusal",
          "[unit][tool][workspace][file_delete]") {
  TempDir root{"oran-workspace-file-delete"};
  TempDir outside{"oran-workspace-file-delete-outside"};
  write_text(root.path() / "doomed.txt", "remove");
  write_text(root.path() / "tree" / "leaf.txt", "remove");
  write_text(outside.path() / "secret.txt", "keep");

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_delete(registry).has_value());

    auto workspace = make_workspace(root.path());
    auto rules = allow_tool_rules(std::string{tool::kFileDeleteName}, core::Capability::delete_path);
    permission::RecordingAuditSink sink;
    auto ctx = make_workspace_ctx(io, rules, sink, workspace);

    auto deleted = co_await registry.dispatch(tool::kFileDeleteName, R"({"path":"doomed.txt"})", ctx);
    REQUIRE(deleted.has_value());
    REQUIRE_FALSE(std::filesystem::exists(root.path() / "doomed.txt"));

    auto deleted_tree = co_await registry.dispatch(tool::kFileDeleteName, R"({"path":"tree","recursive":true})", ctx);
    REQUIRE(deleted_tree.has_value());
    REQUIRE_FALSE(std::filesystem::exists(root.path() / "tree"));

    std::error_code ec;
    const auto outside_relative_path = std::filesystem::relative(outside.path() / "secret.txt", root.path(), ec);
    REQUIRE(ec.value() == 0);
    const auto escaped_input = std::format(R"({{"path":"{}"}})", outside_relative_path.string());
    auto escaped = co_await registry.dispatch(tool::kFileDeleteName, escaped_input, ctx);
    REQUIRE_FALSE(escaped.has_value());
    REQUIRE(escaped.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(context_has(escaped.error(), "reason", "outside_workspace"));
    REQUIRE(std::filesystem::exists(outside.path() / "secret.txt"));
  });
}

TEST_CASE("FileDelete rejects workspace symlink mutation targets", "[unit][tool][workspace][file_delete]") {
  TempDir root{"oran-workspace-file-delete-link"};
  write_text(root.path() / "target.txt", "survives");
  create_symlink_or_skip(root.path() / "target.txt", root.path() / "link.txt");

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_delete(registry).has_value());

    auto workspace = make_workspace(root.path());
    auto rules = allow_tool_rules(std::string{tool::kFileDeleteName}, core::Capability::delete_path);
    permission::RecordingAuditSink sink;
    auto ctx = make_workspace_ctx(io, rules, sink, workspace);

    auto deleted = co_await registry.dispatch(tool::kFileDeleteName, R"({"path":"link.txt"})", ctx);
    REQUIRE_FALSE(deleted.has_value());
    REQUIRE(deleted.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(context_has(deleted.error(), "reason", "symlink_target"));
    REQUIRE(std::filesystem::is_symlink(root.path() / "link.txt"));
    REQUIRE(std::filesystem::exists(root.path() / "target.txt"));
  });
}

TEST_CASE("FileSearch uses DispatchContext workspace for relative searches and traversal refusal",
          "[unit][tool][workspace][file_search]") {
  TempDir root{"oran-workspace-file-search"};
  TempDir outside{"oran-workspace-file-search-outside"};
  write_text(root.path() / "nested" / "note.txt", "alpha\nneedle here\nbeta");
  write_text(outside.path() / "secret.txt", "needle outside");

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_search(registry).has_value());

    auto workspace = make_workspace(root.path());
    auto rules = allow_tool_rules(std::string{tool::kFileSearchName}, core::Capability::read_file);
    permission::RecordingAuditSink sink;
    auto ctx = make_workspace_ctx(io, rules, sink, workspace);

    auto found =
        co_await registry.dispatch(tool::kFileSearchName, R"({"path":"nested/note.txt","pattern":"needle"})", ctx);
    REQUIRE(found.has_value());
    REQUIRE(found->text.contains("<workspace>/nested/note.txt:2:needle here"));
    REQUIRE(found->data_json.has_value());
    const auto found_data = nlohmann::json::parse(*found->data_json);
    REQUIRE(found_data["path"] == "<workspace>/nested/note.txt");
    REQUIRE(found_data["matches"][0]["path"] == "<workspace>/nested/note.txt");

    std::error_code ec;
    const auto outside_relative_path = std::filesystem::relative(outside.path() / "secret.txt", root.path(), ec);
    REQUIRE(ec.value() == 0);
    const auto escaped_input = std::format(R"({{"path":"{}","pattern":"needle"}})", outside_relative_path.string());
    auto escaped = co_await registry.dispatch(tool::kFileSearchName, escaped_input, ctx);
    REQUIRE_FALSE(escaped.has_value());
    REQUIRE(escaped.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(context_has(escaped.error(), "reason", "outside_workspace"));
  });
}

TEST_CASE("FileSearch rejects symlink roots that escape the workspace", "[unit][tool][workspace][file_search]") {
  TempDir root{"oran-workspace-file-search-symlink"};
  TempDir outside{"oran-workspace-file-search-symlink-outside"};
  write_text(outside.path() / "secret.txt", "needle outside");
  create_symlink_or_skip(outside.path() / "secret.txt", root.path() / "outside-link.txt");

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_search(registry).has_value());

    auto workspace = make_workspace(root.path());
    auto rules = allow_tool_rules(std::string{tool::kFileSearchName}, core::Capability::read_file);
    permission::RecordingAuditSink sink;
    auto ctx = make_workspace_ctx(io, rules, sink, workspace);

    auto escaped =
        co_await registry.dispatch(tool::kFileSearchName, R"({"path":"outside-link.txt","pattern":"needle"})", ctx);
    REQUIRE_FALSE(escaped.has_value());
    REQUIRE(escaped.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(context_has(escaped.error(), "reason", "symlink_escape"));
  });
}

TEST_CASE("FileSearch honors extra_read_roots through the workspace seam", "[unit][tool][workspace][file_search]") {
  TempDir root{"oran-workspace-file-search-primary"};
  TempDir readable{"oran-workspace-file-search-readable"};
  write_text(readable.path() / "audit.log", "needle in the override root");

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_search(registry).has_value());

    auto workspace = make_workspace(root.path(),
                                    tool::WorkspaceOptions{
                                        .extra_read_roots = {readable.path().string()},
                                    });
    auto rules = allow_tool_rules(std::string{tool::kFileSearchName}, core::Capability::read_file);
    permission::RecordingAuditSink sink;
    auto ctx = make_workspace_ctx(io, rules, sink, workspace);

    const auto override_input =
        std::format(R"({{"path":"{}","pattern":"needle"}})", (readable.path() / "audit.log").string());
    auto found = co_await registry.dispatch(tool::kFileSearchName, override_input, ctx);
    REQUIRE(found.has_value());
    REQUIRE(found->text.contains("needle in the override root"));
  });
}

TEST_CASE("DirectoryList uses DispatchContext workspace for relative listings and traversal refusal",
          "[unit][tool][workspace][directory_list]") {
  TempDir root{"oran-workspace-directory-list"};
  TempDir outside{"oran-workspace-directory-list-outside"};
  write_text(root.path() / "nested" / "a.txt", "a");
  write_text(root.path() / "nested" / "b.txt", "bb");
  write_text(outside.path() / "secret.txt", "outside");

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_directory_list(registry).has_value());

    auto workspace = make_workspace(root.path());
    auto rules = allow_tool_rules(std::string{tool::kDirectoryListName}, core::Capability::list_directory);
    permission::RecordingAuditSink sink;
    auto ctx = make_workspace_ctx(io, rules, sink, workspace);

    auto listed = co_await registry.dispatch(tool::kDirectoryListName, R"({"path":"nested"})", ctx);
    REQUIRE(listed.has_value());
    REQUIRE(listed->text.contains("<workspace>/nested/a.txt:regular_file:1"));
    REQUIRE(listed->text.contains("<workspace>/nested/b.txt:regular_file:2"));
    REQUIRE(listed->data_json.has_value());
    const auto listed_data = nlohmann::json::parse(*listed->data_json);
    REQUIRE(listed_data["path"] == "<workspace>/nested");
    REQUIRE(listed_data["entries"][0]["path"].get<std::string>().starts_with("<workspace>/nested/"));

    std::error_code ec;
    const auto outside_relative_path = std::filesystem::relative(outside.path(), root.path(), ec);
    REQUIRE(ec.value() == 0);
    const auto escaped_input = std::format(R"({{"path":"{}"}})", outside_relative_path.string());
    auto escaped = co_await registry.dispatch(tool::kDirectoryListName, escaped_input, ctx);
    REQUIRE_FALSE(escaped.has_value());
    REQUIRE(escaped.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(context_has(escaped.error(), "reason", "outside_workspace"));
  });
}

TEST_CASE("DirectoryList rejects symlink roots that escape the workspace", "[unit][tool][workspace][directory_list]") {
  TempDir root{"oran-workspace-directory-list-symlink"};
  TempDir outside{"oran-workspace-directory-list-symlink-outside"};
  write_text(outside.path() / "secret.txt", "outside");
  create_symlink_or_skip(outside.path(), root.path() / "outside-link");

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_directory_list(registry).has_value());

    auto workspace = make_workspace(root.path());
    auto rules = allow_tool_rules(std::string{tool::kDirectoryListName}, core::Capability::list_directory);
    permission::RecordingAuditSink sink;
    auto ctx = make_workspace_ctx(io, rules, sink, workspace);

    auto escaped = co_await registry.dispatch(tool::kDirectoryListName, R"({"path":"outside-link"})", ctx);
    REQUIRE_FALSE(escaped.has_value());
    REQUIRE(escaped.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(context_has(escaped.error(), "reason", "symlink_escape"));
  });
}
