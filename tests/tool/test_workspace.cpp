// tests/tool/test_workspace.cpp — workspace resolver coverage.

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/core/error.hpp>
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
  rules.add(permission::Rule{.verdict = permission::Verdict::allow, .tool_pattern = "file.read"});
  return rules;
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

TEST_CASE("file.read uses DispatchContext workspace when supplied", "[unit][tool][workspace][file_read]") {
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
    auto ctx = tool::DispatchContext{
        .executor = io.get_executor(),
        .mode = permission::Mode::default_,
        .rules = rules,
        .audit = sink,
        .workspace = &workspace,
        .scope_key = "scope-A",
        .agent_key = "coder",
        .identity = "operator-1",
    };

    auto read = co_await registry.dispatch("file.read", R"({"path":"note.txt"})", ctx);
    REQUIRE(read.has_value());
    REQUIRE(read->text == "inside");

    std::error_code ec;
    const auto outside_relative_path = std::filesystem::relative(outside.path() / "secret.txt", root.path(), ec);
    REQUIRE(ec.value() == 0);
    const auto outside_relative = outside_relative_path.string();
    const auto escaped_input = std::format(R"({{"path":"{}"}})", outside_relative);
    auto escaped = co_await registry.dispatch("file.read", escaped_input, ctx);
    REQUIRE_FALSE(escaped.has_value());
    REQUIRE(escaped.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(context_has(escaped.error(), "reason", "outside_workspace"));
  });
}
