// tests/bootstrap/test_runtime_assembly.cpp — per-process permission + audit assembly coverage.

#include <chrono>
#include <filesystem>
#include <string>
#include <utility>

#include <asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/bootstrap.hpp>
#include <oran/core/error.hpp>
#include <oran/core/time.hpp>
#include <oran/permission.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace bootstrap = orangutan::bootstrap;
namespace core = orangutan::core;
namespace permission = orangutan::permission;
namespace test = orangutan::tests;

namespace {

class TempDir {
public:
  explicit TempDir(std::string name)
      : path_(std::filesystem::temp_directory_path() /
              (std::move(name) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
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

/// RAII chdir guard. The built-in-migration path must not depend on CWD —
/// the slice-15 packaging swap pulled the SQL into the binary precisely
/// so a process launched from anywhere can provision audit.db. This guard
/// gives the test below a way to assert that property without leaking the
/// CWD change into sibling cases.
class CwdGuard {
public:
  explicit CwdGuard(const std::filesystem::path& destination) : original_(std::filesystem::current_path()) {
    std::filesystem::current_path(destination);
  }

  ~CwdGuard() {
    std::error_code ec;
    std::filesystem::current_path(original_, ec);
  }

  CwdGuard(const CwdGuard&) = delete;
  CwdGuard& operator=(const CwdGuard&) = delete;

private:
  std::filesystem::path original_;
};

permission::AuditEvent make_event(std::string scope, std::string tool, permission::AuditOutcome outcome) {
  permission::AuditEvent event;
  event.scope_key = std::move(scope);
  event.agent_key = "coder";
  event.tool_name = std::move(tool);
  event.identity = "operator-1";
  event.verdict = permission::Verdict::allow;
  event.outcome = outcome;
  event.reason = "test rule";
  return event;
}

[[nodiscard]] core::Time fixed_now() noexcept {
  using namespace std::chrono;
  return core::Time{sys_days{year{2026} / January / day{1}}};
}

}  // namespace

TEST_CASE("RuntimeAssembly::build installs NullAuditSink when audit disabled", "[unit][bootstrap][runtime_assembly]") {
  TempDir temp{"oran-assembly-null"};
  asio::io_context io;

  auto assembly = bootstrap::RuntimeAssembly::build(temp.path().string(),
                                                    io.get_executor(),
                                                    bootstrap::RuntimeAssemblyOptions{.audit_enabled = false});
  REQUIRE(assembly.has_value());
  REQUIRE_FALSE(assembly->audit_enabled());
  REQUIRE(assembly->audit_path().empty());
  REQUIRE_FALSE(std::filesystem::exists(temp.path() / ".orangutan" / "audit.db"));
}

TEST_CASE("RuntimeAssembly::build provisions audit.db at the workspace default path",
          "[unit][bootstrap][runtime_assembly]") {
  TempDir temp{"oran-assembly-default-path"};
  asio::io_context io;

  auto assembly = bootstrap::RuntimeAssembly::build(temp.path().string(), io.get_executor());
  REQUIRE(assembly.has_value());
  REQUIRE(assembly->audit_enabled());
  REQUIRE(assembly->audit_path() == (temp.path() / ".orangutan" / "audit.db").string());
  REQUIRE(std::filesystem::exists(temp.path() / ".orangutan" / "audit.db"));
}

TEST_CASE("RuntimeAssembly::build honors an explicit audit DB path", "[unit][bootstrap][runtime_assembly]") {
  TempDir temp{"oran-assembly-explicit-path"};
  const auto explicit_path = (temp.path() / "nested" / "audit.db").string();
  asio::io_context io;

  auto assembly = bootstrap::RuntimeAssembly::build(temp.path().string(),
                                                    io.get_executor(),
                                                    bootstrap::RuntimeAssemblyOptions{.audit_db_path = explicit_path});
  REQUIRE(assembly.has_value());
  REQUIRE(assembly->audit_path() == explicit_path);
  REQUIRE(std::filesystem::exists(explicit_path));
}

TEST_CASE("RuntimeAssembly::build is idempotent on re-run", "[unit][bootstrap][runtime_assembly]") {
  TempDir temp{"oran-assembly-idempotent"};
  asio::io_context io;

  auto first = bootstrap::RuntimeAssembly::build(temp.path().string(), io.get_executor());
  REQUIRE(first.has_value());

  asio::io_context io2;
  auto second = bootstrap::RuntimeAssembly::build(temp.path().string(), io2.get_executor());
  REQUIRE(second.has_value());
  REQUIRE(std::filesystem::exists(temp.path() / ".orangutan" / "audit.db"));
}

TEST_CASE("RuntimeAssembly::build rejects an empty workspace", "[unit][bootstrap][runtime_assembly]") {
  asio::io_context io;
  auto assembly = bootstrap::RuntimeAssembly::build({}, io.get_executor());
  REQUIRE_FALSE(assembly.has_value());
  REQUIRE(assembly.error().kind() == core::ErrorKind::invalid_argument);
}

TEST_CASE("RuntimeAssembly storage sink records events end-to-end", "[unit][bootstrap][runtime_assembly]") {
  TempDir temp{"oran-assembly-storage-record"};

  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto built = bootstrap::RuntimeAssembly::build(temp.path().string(), io.get_executor());
    REQUIRE(built.has_value());
    auto assembly = std::move(*built);

    auto recorded =
        co_await assembly.audit_sink().record(make_event("scope-A", "file.read", permission::AuditOutcome::allow));
    REQUIRE(recorded.has_value());
  });
}

TEST_CASE("RuntimeAssembly null sink discards events silently", "[unit][bootstrap][runtime_assembly]") {
  TempDir temp{"oran-assembly-null-record"};

  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto built = bootstrap::RuntimeAssembly::build(temp.path().string(),
                                                   io.get_executor(),
                                                   bootstrap::RuntimeAssemblyOptions{.audit_enabled = false});
    REQUIRE(built.has_value());
    auto assembly = std::move(*built);

    auto recorded =
        co_await assembly.audit_sink().record(make_event("scope-A", "file.read", permission::AuditOutcome::allow));
    REQUIRE(recorded.has_value());
  });
}

TEST_CASE("RuntimeAssembly approval broker round-trips a fresh token", "[unit][bootstrap][runtime_assembly]") {
  TempDir temp{"oran-assembly-broker"};
  asio::io_context io;

  auto built = bootstrap::RuntimeAssembly::build(temp.path().string(),
                                                 io.get_executor(),
                                                 bootstrap::RuntimeAssemblyOptions{.audit_enabled = false});
  REQUIRE(built.has_value());
  auto assembly = std::move(*built);

  const auto now = fixed_now();
  permission::ApprovalGrant grant{
      .tool_name = "file.write",
      .input = "/tmp/note.txt",
      .identity = "operator-1",
      .ttl = std::chrono::seconds{60},
      .replay_max = 2,
  };
  auto token = assembly.approval_broker().approve(grant, now);
  REQUIRE(token.tool_name == "file.write");

  auto check = assembly.approval_broker().check(token, "file.write", "/tmp/note.txt", "operator-1", now);
  REQUIRE(check.has_value());
}

TEST_CASE("RuntimeAssembly approval broker rejects a cross-tool token", "[unit][bootstrap][runtime_assembly]") {
  TempDir temp{"oran-assembly-broker-cross-tool"};
  asio::io_context io;

  auto built = bootstrap::RuntimeAssembly::build(temp.path().string(),
                                                 io.get_executor(),
                                                 bootstrap::RuntimeAssemblyOptions{.audit_enabled = false});
  REQUIRE(built.has_value());
  auto assembly = std::move(*built);

  const auto now = fixed_now();
  permission::ApprovalGrant grant{
      .tool_name = "file.write",
      .input = "/tmp/note.txt",
      .identity = "operator-1",
  };
  auto token = assembly.approval_broker().approve(grant, now);

  auto cross_tool = assembly.approval_broker().check(token, "file.delete", "/tmp/note.txt", "operator-1", now);
  REQUIRE_FALSE(cross_tool.has_value());
  REQUIRE(cross_tool.error().kind() == core::ErrorKind::permission_denied);
}

TEST_CASE("RuntimeAssembly::build provisions audit.db from a non-source CWD", "[unit][bootstrap][runtime_assembly]") {
  TempDir workspace{"oran-assembly-cwd-workspace"};
  TempDir cwd{"oran-assembly-cwd-elsewhere"};
  CwdGuard guard{cwd.path()};
  asio::io_context io;

  auto built = bootstrap::RuntimeAssembly::build(workspace.path().string(), io.get_executor());
  REQUIRE(built.has_value());
  REQUIRE(built->audit_enabled());
  REQUIRE(std::filesystem::exists(workspace.path() / ".orangutan" / "audit.db"));
}
