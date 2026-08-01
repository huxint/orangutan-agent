// tests/io/test_directory_authority.cpp — dirfd authority regressions.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include <oran/io/directory_authority.hpp>
#include <oran/io/file.hpp>
#include <oran/io/fingerprint.hpp>

#include "../test-helpers/run_async.hpp"

namespace core = orangutan::core;
namespace io = orangutan::io;
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

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

void write_direct(const std::filesystem::path& path, std::string_view contents) {
  std::ofstream output{path, std::ios::binary};
  output << contents;
}

[[nodiscard]] std::string read_direct(const std::filesystem::path& path) {
  std::ifstream input{path, std::ios::binary};
  return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void require_no_mutation_temp(const std::filesystem::path& directory) {
  for (const auto& entry : std::filesystem::directory_iterator{directory}) {
    REQUIRE_FALSE(entry.path().filename().string().starts_with(".orangutan.tmp."));
  }
}

[[nodiscard]] std::string read_handle(const io::ReadOnlyFile& file) {
  std::array<char, 64> buffer{};
  const auto count = ::read(file.native_handle(), buffer.data(), buffer.size());
  REQUIRE(count >= 0);
  return std::string{buffer.data(), static_cast<std::size_t>(count)};
}

}  // namespace

TEST_CASE("DirectoryAuthority retains the opened root across pathname replacement", "[unit][io][authority]") {
  TempDir temp{"oran-io-authority-root-replacement"};
  const auto workspace = temp.path() / "workspace";
  const auto moved_workspace = temp.path() / "workspace-moved";
  const auto outside = temp.path() / "outside";
  std::filesystem::create_directories(workspace / "nested");
  std::filesystem::create_directories(outside / "nested");
  write_direct(workspace / "nested" / "note.txt", "inside");
  write_direct(outside / "nested" / "note.txt", "outside");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());

  std::filesystem::rename(workspace, moved_workspace);
  std::filesystem::create_directory_symlink(outside, workspace);

  auto child = authority->open_directory(io::AnchoredPath{
      .relative_path = "nested",
      .symlink_policy = io::AnchoredSymlinkPolicy::allow_beneath,
  });
  REQUIRE(child.has_value());

  auto opened = child->open_file(io::AnchoredPath{
      .relative_path = "note.txt",
      .symlink_policy = io::AnchoredSymlinkPolicy::allow_beneath,
  });
  REQUIRE(opened.has_value());
  REQUIRE(read_handle(*opened) == "inside");
  auto fingerprint = io::compute_file_fingerprint(*opened);
  REQUIRE(fingerprint.has_value());
  CHECK(fingerprint->size_bytes == 6);

  std::ifstream redirected{workspace / "nested" / "note.txt", std::ios::binary};
  std::string redirected_text;
  redirected >> redirected_text;
  REQUIRE(redirected_text == "outside");
}

TEST_CASE("list_directory through an authority returns sorted visible entry metadata",
          "[unit][io][authority][directory]") {
  TempDir temp{"oran-io-authority-list"};
  const auto workspace = temp.path() / "workspace";
  std::filesystem::create_directories(workspace / "a-dir");
  write_direct(workspace / "b.txt", "bb");
  write_direct(workspace / ".hidden", "hidden");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto listed = co_await io::list_directory(context.get_executor(), *authority);
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 2);
    REQUIRE((*listed)[0].name == "a-dir");
    REQUIRE((*listed)[0].kind == io::DirectoryEntryKind::directory);
    REQUIRE_FALSE((*listed)[0].size_bytes.has_value());
    REQUIRE((*listed)[1].name == "b.txt");
    REQUIRE((*listed)[1].kind == io::DirectoryEntryKind::regular_file);
    REQUIRE((*listed)[1].size_bytes == 2);
  });
}

TEST_CASE("list_directory through an authority can include hidden entries", "[unit][io][authority][directory]") {
  TempDir temp{"oran-io-authority-list-hidden"};
  const auto workspace = temp.path() / "workspace";
  std::filesystem::create_directory(workspace);
  write_direct(workspace / ".hidden", "hidden");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto listed = co_await io::list_directory(context.get_executor(),
                                              *authority,
                                              io::ListDirectoryOptions{.include_hidden = true});
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 1);
    REQUIRE((*listed)[0].name == ".hidden");
    REQUIRE((*listed)[0].kind == io::DirectoryEntryKind::regular_file);
    REQUIRE((*listed)[0].size_bytes == 6);
  });
}

TEST_CASE("list_directory through an authority enforces max_entries", "[unit][io][authority][directory]") {
  TempDir temp{"oran-io-authority-list-limit"};
  const auto workspace = temp.path() / "workspace";
  std::filesystem::create_directory(workspace);
  write_direct(workspace / "a.txt", "a");
  write_direct(workspace / "b.txt", "b");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto listed =
        co_await io::list_directory(context.get_executor(), *authority, io::ListDirectoryOptions{.max_entries = 1});
    REQUIRE_FALSE(listed.has_value());
    REQUIRE(listed.error().kind() == core::ErrorKind::io);
  });
}

TEST_CASE("list_directory keeps the authority root across pathname replacement", "[unit][io][authority][directory]") {
  TempDir temp{"oran-io-authority-list-root-replacement"};
  const auto workspace = temp.path() / "workspace";
  const auto moved_workspace = temp.path() / "workspace-moved";
  const auto outside = temp.path() / "outside";
  std::filesystem::create_directory(workspace);
  std::filesystem::create_directory(outside);
  write_direct(workspace / "inside.txt", "inside");
  write_direct(outside / "outside.txt", "outside");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());

  std::filesystem::rename(workspace, moved_workspace);
  std::filesystem::create_directory_symlink(outside, workspace);

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto listed = co_await io::list_directory(context.get_executor(), *authority);
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 1);
    REQUIRE((*listed)[0].name == "inside.txt");
    REQUIRE((*listed)[0].kind == io::DirectoryEntryKind::regular_file);
    REQUIRE((*listed)[0].size_bytes == 6);
  });

  REQUIRE(std::filesystem::exists(outside / "outside.txt"));
}

TEST_CASE("DirectoryAuthority rejects traversal and symlink escapes", "[unit][io][authority]") {
  TempDir temp{"oran-io-authority-confinement"};
  const auto workspace = temp.path() / "workspace";
  const auto outside = temp.path() / "outside";
  std::filesystem::create_directories(workspace);
  std::filesystem::create_directories(outside);
  write_direct(outside / "secret.txt", "secret");
  std::filesystem::create_directory_symlink(outside, workspace / "escape");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());

  auto traversal = authority->open_file(io::AnchoredPath{
      .relative_path = "../outside/secret.txt",
      .symlink_policy = io::AnchoredSymlinkPolicy::allow_beneath,
  });
  REQUIRE_FALSE(traversal.has_value());
  REQUIRE(traversal.error().kind() == core::ErrorKind::permission_denied);

  auto absolute = authority->open_file(io::AnchoredPath{
      .relative_path = (outside / "secret.txt").string(),
      .symlink_policy = io::AnchoredSymlinkPolicy::allow_beneath,
  });
  REQUIRE_FALSE(absolute.has_value());
  REQUIRE(absolute.error().kind() == core::ErrorKind::invalid_argument);

  auto symlink_escape = authority->open_file(io::AnchoredPath{
      .relative_path = "escape/secret.txt",
      .symlink_policy = io::AnchoredSymlinkPolicy::allow_beneath,
  });
  REQUIRE_FALSE(symlink_escape.has_value());
  REQUIRE(symlink_escape.error().kind() == core::ErrorKind::permission_denied);
}

TEST_CASE("DirectoryAuthority reject-all policy refuses an in-root symlink", "[unit][io][authority]") {
  TempDir temp{"oran-io-authority-symlink-policy"};
  const auto workspace = temp.path() / "workspace";
  std::filesystem::create_directories(workspace);
  write_direct(workspace / "real.txt", "inside");
  std::filesystem::create_symlink("real.txt", workspace / "link.txt");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());

  auto rejected = authority->open_file(io::AnchoredPath{
      .relative_path = "link.txt",
      .symlink_policy = io::AnchoredSymlinkPolicy::reject_all,
  });
  REQUIRE_FALSE(rejected.has_value());
  REQUIRE(rejected.error().kind() == core::ErrorKind::permission_denied);
}

TEST_CASE("FileMutation refuses symlink components and targets", "[unit][io][authority][mutation][symlink]") {
  TempDir temp{"oran-io-mutation-symlinks"};
  const auto workspace = temp.path() / "workspace";
  std::filesystem::create_directories(workspace / "real");
  write_direct(workspace / "real" / "note.txt", "inside");
  std::filesystem::create_directory_symlink("real", workspace / "parent-link");
  std::filesystem::create_symlink("real/note.txt", workspace / "target-link.txt");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());

  auto parent_symlink = authority->begin_file_mutation("parent-link/note.txt");
  REQUIRE_FALSE(parent_symlink.has_value());
  REQUIRE(parent_symlink.error().kind() == core::ErrorKind::permission_denied);

  auto target_symlink = authority->begin_file_mutation("target-link.txt");
  REQUIRE_FALSE(target_symlink.has_value());
  REQUIRE(target_symlink.error().kind() == core::ErrorKind::permission_denied);
}

TEST_CASE("DeleteMutation removes an anchored regular file", "[unit][io][authority][delete]") {
  TempDir temp{"oran-io-delete-file"};
  const auto workspace = temp.path() / "workspace";
  const auto target = workspace / "removable.txt";
  std::filesystem::create_directory(workspace);
  write_direct(target, "bye");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_delete("removable.txt");
  REQUIRE(mutation.has_value());

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto deleted = co_await io::delete_path(context.get_executor(), std::move(*mutation));
    REQUIRE(deleted.has_value());
    REQUIRE(deleted->paths_removed == std::uintmax_t{1});
  });

  REQUIRE_FALSE(std::filesystem::exists(target));
}

TEST_CASE("DeleteMutation recursively removes an anchored directory tree", "[unit][io][authority][delete]") {
  TempDir temp{"oran-io-delete-tree"};
  const auto workspace = temp.path() / "workspace";
  const auto tree = workspace / "tree";
  std::filesystem::create_directories(tree / "nested");
  write_direct(tree / "top.txt", "bye");
  write_direct(tree / "nested" / "leaf.txt", "bye");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_delete("tree");
  REQUIRE(mutation.has_value());

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto deleted = co_await io::delete_path(context.get_executor(),
                                            std::move(*mutation),
                                            io::DeletePathOptions{.recursive = true});
    REQUIRE(deleted.has_value());
    REQUIRE(deleted->paths_removed == std::uintmax_t{4});
  });

  REQUIRE_FALSE(std::filesystem::exists(tree));
}

TEST_CASE("DeleteMutation refuses a directory without recursive intent", "[unit][io][authority][delete]") {
  TempDir temp{"oran-io-delete-no-recursive"};
  const auto workspace = temp.path() / "workspace";
  const auto tree = workspace / "tree";
  std::filesystem::create_directories(tree);

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_delete("tree");
  REQUIRE(mutation.has_value());

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto deleted = co_await io::delete_path(context.get_executor(), std::move(*mutation));
    REQUIRE_FALSE(deleted.has_value());
    REQUIRE(deleted.error().kind() == core::ErrorKind::invalid_argument);
  });

  REQUIRE(std::filesystem::is_directory(tree));
}

TEST_CASE("DeleteMutation refuses a top-level symlink", "[unit][io][authority][delete][symlink]") {
  TempDir temp{"oran-io-delete-top-symlink"};
  const auto workspace = temp.path() / "workspace";
  const auto target = workspace / "target";
  const auto link = workspace / "link";
  std::filesystem::create_directories(target);
  write_direct(target / "survives.txt", "still here");
  std::error_code link_ec;
  std::filesystem::create_directory_symlink("target", link, link_ec);
  if (link_ec) {
    SUCCEED("symlink creation not supported on this filesystem: " << link_ec.message());
    return;
  }

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_delete("link");
  REQUIRE_FALSE(mutation.has_value());
  REQUIRE(mutation.error().kind() == core::ErrorKind::permission_denied);
  REQUIRE(std::filesystem::is_symlink(link));
  REQUIRE(std::filesystem::exists(target / "survives.txt"));
}

TEST_CASE("DeleteMutation recursive delete does not follow nested symlinks", "[unit][io][authority][delete][symlink]") {
  TempDir temp{"oran-io-delete-nested-symlink"};
  TempDir outside{"oran-io-delete-nested-symlink-target"};
  const auto workspace = temp.path() / "workspace";
  const auto tree = workspace / "tree";
  const auto outside_target = outside.path() / "target";
  std::filesystem::create_directories(tree);
  std::filesystem::create_directories(outside_target);
  write_direct(outside_target / "survives.txt", "still here");
  std::error_code link_ec;
  std::filesystem::create_directory_symlink(outside_target, tree / "outside-link", link_ec);
  if (link_ec) {
    SUCCEED("symlink creation not supported on this filesystem: " << link_ec.message());
    return;
  }

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_delete("tree");
  REQUIRE(mutation.has_value());

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto deleted = co_await io::delete_path(context.get_executor(),
                                            std::move(*mutation),
                                            io::DeletePathOptions{.recursive = true});
    REQUIRE(deleted.has_value());
  });

  REQUIRE_FALSE(std::filesystem::exists(tree));
  REQUIRE(std::filesystem::exists(outside_target / "survives.txt"));
}

TEST_CASE("DeleteMutation recursively removes non-directory special entries", "[unit][io][authority][delete]") {
  TempDir temp{"oran-io-delete-special-entry"};
  const auto workspace = temp.path() / "workspace";
  const auto tree = workspace / "tree";
  const auto fifo = tree / "events.fifo";
  std::filesystem::create_directories(tree);
  REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_delete("tree");
  REQUIRE(mutation.has_value());

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto deleted = co_await io::delete_path(context.get_executor(),
                                            std::move(*mutation),
                                            io::DeletePathOptions{.recursive = true});
    REQUIRE(deleted.has_value());
    REQUIRE(deleted->paths_removed == std::uintmax_t{2});
  });

  REQUIRE_FALSE(std::filesystem::exists(tree));
}

TEST_CASE("DeleteMutation retains its authority across root replacement", "[unit][io][authority][delete]") {
  TempDir temp{"oran-io-delete-root-replacement"};
  const auto workspace = temp.path() / "workspace";
  const auto moved_workspace = temp.path() / "workspace-moved";
  const auto outside = temp.path() / "outside";
  std::filesystem::create_directory(workspace);
  std::filesystem::create_directory(outside);
  write_direct(workspace / "note.txt", "inside");
  write_direct(outside / "note.txt", "outside");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  std::filesystem::rename(workspace, moved_workspace);
  std::filesystem::create_directory_symlink(outside, workspace);

  auto mutation = authority->begin_delete("note.txt");
  REQUIRE(mutation.has_value());
  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto deleted = co_await io::delete_path(context.get_executor(), std::move(*mutation));
    REQUIRE(deleted.has_value());
  });

  REQUIRE_FALSE(std::filesystem::exists(moved_workspace / "note.txt"));
  REQUIRE(read_direct(outside / "note.txt") == "outside");
}

TEST_CASE("DeleteMutation rejects target replacement before commit", "[unit][io][authority][delete][conflict]") {
  TempDir temp{"oran-io-delete-target-replacement"};
  const auto workspace = temp.path() / "workspace";
  const auto target = workspace / "note.txt";
  std::filesystem::create_directory(workspace);
  write_direct(target, "original");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_delete("note.txt");
  REQUIRE(mutation.has_value());

  const auto replacement = workspace / "replacement.txt";
  write_direct(replacement, "external-version");
  std::filesystem::rename(replacement, target);

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto deleted = co_await io::delete_path(context.get_executor(), std::move(*mutation));
    REQUIRE_FALSE(deleted.has_value());
    REQUIRE(deleted.error().kind() == core::ErrorKind::conflict);
  });

  REQUIRE(read_direct(target) == "external-version");
}

TEST_CASE("FileMutation creates parent directories and appends", "[unit][io][authority][mutation]") {
  TempDir temp{"oran-io-mutation-write"};
  const auto workspace = temp.path() / "workspace";
  const auto target = workspace / "nested" / "output.txt";
  std::filesystem::create_directory(workspace);

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto created = authority->begin_file_mutation("nested/output.txt", true);
  REQUIRE(created.has_value());

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto first = co_await io::write_text_file(context.get_executor(),
                                              std::move(*created),
                                              "one",
                                              io::WriteTextOptions{.atomic = true});
    REQUIRE(first.has_value());

    auto appended = authority->begin_file_mutation("nested/output.txt");
    REQUIRE(appended.has_value());
    auto second = co_await io::write_text_file(context.get_executor(),
                                               std::move(*appended),
                                               "\ntwo",
                                               io::WriteTextOptions{.mode = io::WriteMode::append});
    REQUIRE(second.has_value());
  });

  REQUIRE(read_direct(target) == "one\ntwo");
}

TEST_CASE("FileMutation refuses fail-if-exists overwrites", "[unit][io][authority][mutation]") {
  TempDir temp{"oran-io-mutation-exclusive"};
  const auto workspace = temp.path() / "workspace";
  const auto target = workspace / "output.txt";
  std::filesystem::create_directory(workspace);
  write_direct(target, "existing");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_file_mutation("output.txt");
  REQUIRE(mutation.has_value());

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto result = co_await io::write_text_file(context.get_executor(),
                                               std::move(*mutation),
                                               "new",
                                               io::WriteTextOptions{.mode = io::WriteMode::fail_if_exists});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::conflict);
  });

  REQUIRE(read_direct(target) == "existing");
}

TEST_CASE("FileMutation atomically replaces a file without leaving a temp", "[unit][io][authority][mutation][atomic]") {
  TempDir temp{"oran-io-mutation-atomic"};
  const auto workspace = temp.path() / "workspace";
  const auto target = workspace / "data.txt";
  std::filesystem::create_directory(workspace);
  write_direct(target, "old contents");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_file_mutation("data.txt");
  REQUIRE(mutation.has_value());

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto result = co_await io::write_text_file(context.get_executor(),
                                               std::move(*mutation),
                                               "new contents",
                                               io::WriteTextOptions{.atomic = true});
    REQUIRE(result.has_value());
  });

  REQUIRE(read_direct(target) == "new contents");
  require_no_mutation_temp(workspace);
}

TEST_CASE("FileMutation does not reuse legacy counter temp names", "[unit][io][authority][mutation][atomic]") {
  TempDir temp{"oran-io-mutation-temp-name"};
  const auto workspace = temp.path() / "workspace";
  const auto target = workspace / "data.txt";
  const auto legacy_temp = workspace / ".data.txt.orangutan.tmp.0";
  std::filesystem::create_directory(workspace);
  write_direct(target, "old contents");
  write_direct(legacy_temp, "legacy marker");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_file_mutation("data.txt");
  REQUIRE(mutation.has_value());

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto result = co_await io::write_text_file(context.get_executor(),
                                               std::move(*mutation),
                                               "new contents",
                                               io::WriteTextOptions{.atomic = true});
    REQUIRE(result.has_value());
  });

  REQUIRE(read_direct(target) == "new contents");
  REQUIRE(read_direct(legacy_temp) == "legacy marker");
}

TEST_CASE("FileMutation supports atomic durability modes", "[unit][io][authority][mutation][atomic]") {
  TempDir temp{"oran-io-mutation-durable"};
  const auto workspace = temp.path() / "workspace";
  const auto file_only = workspace / "file-only.txt";
  const auto file_and_parent = workspace / "file-and-parent.txt";
  std::filesystem::create_directory(workspace);

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto file_mutation = authority->begin_file_mutation("file-only.txt");
  auto parent_mutation = authority->begin_file_mutation("file-and-parent.txt");
  REQUIRE(file_mutation.has_value());
  REQUIRE(parent_mutation.has_value());

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto synced_file = co_await io::write_text_file(
        context.get_executor(),
        std::move(*file_mutation),
        "file durable",
        io::WriteTextOptions{.atomic = true, .durability = io::WriteTextDurability::fsync_file});
    REQUIRE(synced_file.has_value());

    auto synced_parent = co_await io::write_text_file(
        context.get_executor(),
        std::move(*parent_mutation),
        "parent durable",
        io::WriteTextOptions{.atomic = true, .durability = io::WriteTextDurability::fsync_file_and_parent});
    REQUIRE(synced_parent.has_value());
  });

  REQUIRE(read_direct(file_only) == "file durable");
  REQUIRE(read_direct(file_and_parent) == "parent durable");
}

TEST_CASE("FileMutation rejects durability without atomic mode before I/O", "[unit][io][authority][mutation][atomic]") {
  TempDir temp{"oran-io-mutation-durable-reject"};
  const auto workspace = temp.path() / "workspace";
  const auto target = workspace / "data.txt";
  std::filesystem::create_directory(workspace);

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_file_mutation("data.txt");
  REQUIRE(mutation.has_value());

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto result = co_await io::write_text_file(context.get_executor(),
                                               std::move(*mutation),
                                               "contents",
                                               io::WriteTextOptions{.durability = io::WriteTextDurability::fsync_file});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  });

  REQUIRE_FALSE(std::filesystem::exists(target));
  require_no_mutation_temp(workspace);
}

TEST_CASE("FileMutation rejects atomic append and fail-if-exists before I/O",
          "[unit][io][authority][mutation][atomic]") {
  TempDir temp{"oran-io-mutation-bad-mode"};
  const auto workspace = temp.path() / "workspace";
  const auto target = workspace / "data.txt";
  std::filesystem::create_directory(workspace);

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto append_mutation = authority->begin_file_mutation("data.txt");
  REQUIRE(append_mutation.has_value());

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto appended = co_await io::write_text_file(context.get_executor(),
                                                 std::move(*append_mutation),
                                                 "x",
                                                 io::WriteTextOptions{.mode = io::WriteMode::append, .atomic = true});
    REQUIRE_FALSE(appended.has_value());
    REQUIRE(appended.error().kind() == core::ErrorKind::invalid_argument);

    auto exclusive_mutation = authority->begin_file_mutation("data.txt");
    REQUIRE(exclusive_mutation.has_value());
    auto fail_if_exists =
        co_await io::write_text_file(context.get_executor(),
                                     std::move(*exclusive_mutation),
                                     "x",
                                     io::WriteTextOptions{.mode = io::WriteMode::fail_if_exists, .atomic = true});
    REQUIRE_FALSE(fail_if_exists.has_value());
    REQUIRE(fail_if_exists.error().kind() == core::ErrorKind::invalid_argument);
  });

  REQUIRE_FALSE(std::filesystem::exists(target));
  require_no_mutation_temp(workspace);
}

TEST_CASE("DirectoryAuthority pathname identity does not follow a final symlink", "[unit][io][authority][identity]") {
  TempDir temp{"oran-io-authority-path-identity"};
  const auto workspace = temp.path() / "workspace";
  const auto moved_workspace = temp.path() / "workspace-moved";
  std::filesystem::create_directory(workspace);

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  REQUIRE(authority->refers_to_path(workspace.string()).value());

  std::filesystem::rename(workspace, moved_workspace);
  REQUIRE(authority->refers_to_path(moved_workspace.string()).value());
  REQUIRE_FALSE(authority->refers_to_path(workspace.string()).value());

  std::filesystem::create_directory_symlink(moved_workspace, workspace);
  REQUIRE_FALSE(authority->refers_to_path(workspace.string()).value());
}

TEST_CASE("FileMutation retains its parent authority across root replacement", "[unit][io][authority][mutation]") {
  TempDir temp{"oran-io-mutation-root-replacement"};
  const auto workspace = temp.path() / "workspace";
  const auto moved_workspace = temp.path() / "workspace-moved";
  const auto outside = temp.path() / "outside";
  std::filesystem::create_directories(workspace);
  std::filesystem::create_directories(outside);
  write_direct(workspace / "note.txt", "inside-old");
  write_direct(outside / "note.txt", "outside-old");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_file_mutation("note.txt");
  REQUIRE(mutation.has_value());

  std::filesystem::rename(workspace, moved_workspace);
  std::filesystem::create_directory_symlink(outside, workspace);

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto written = co_await io::write_text_file(context.get_executor(),
                                                std::move(*mutation),
                                                "inside-new",
                                                io::WriteTextOptions{.atomic = true});
    REQUIRE(written.has_value());
  });

  std::ifstream inside{moved_workspace / "note.txt", std::ios::binary};
  std::ifstream outside_input{outside / "note.txt", std::ios::binary};
  REQUIRE(std::string{std::istreambuf_iterator<char>{inside}, std::istreambuf_iterator<char>{}} == "inside-new");
  REQUIRE(std::string{std::istreambuf_iterator<char>{outside_input}, std::istreambuf_iterator<char>{}} ==
          "outside-old");
}

TEST_CASE("FileMutation rejects a target change before commit and cleans its temp",
          "[unit][io][authority][mutation][conflict]") {
  TempDir temp{"oran-io-mutation-conflict"};
  const auto workspace = temp.path() / "workspace";
  std::filesystem::create_directories(workspace);
  write_direct(workspace / "note.txt", "original");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_file_mutation("note.txt");
  REQUIRE(mutation.has_value());

  const auto replacement = workspace / "replacement.txt";
  write_direct(replacement, "external-version");
  std::filesystem::rename(replacement, workspace / "note.txt");

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto written = co_await io::write_text_file(context.get_executor(),
                                                std::move(*mutation),
                                                "agent-version",
                                                io::WriteTextOptions{.atomic = true});
    REQUIRE_FALSE(written.has_value());
    REQUIRE(written.error().kind() == core::ErrorKind::conflict);
  });

  std::ifstream current{workspace / "note.txt", std::ios::binary};
  REQUIRE(std::string{std::istreambuf_iterator<char>{current}, std::istreambuf_iterator<char>{}} == "external-version");
  for (const auto& entry : std::filesystem::directory_iterator{workspace}) {
    REQUIRE(entry.path().filename().string().find(".orangutan.tmp.") == std::string::npos);
  }
}

TEST_CASE("FileMutation detects same-size rewrites with restored mtime", "[unit][io][authority][mutation][conflict]") {
  TempDir temp{"oran-io-mutation-ctime-conflict"};
  const auto workspace = temp.path() / "workspace";
  const auto target = workspace / "note.txt";
  std::filesystem::create_directories(workspace);
  write_direct(target, "original");

  struct stat before{};
  REQUIRE(::stat(target.c_str(), &before) == 0);

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_file_mutation("note.txt");
  REQUIRE(mutation.has_value());

  struct stat changed{};
  bool ctime_changed = false;
  for (std::size_t attempt = 0; attempt < 32U && !ctime_changed; ++attempt) {
    write_direct(target, "external");
    const timespec times[] = {before.st_atim, before.st_mtim};
    REQUIRE(::utimensat(AT_FDCWD, target.c_str(), times, 0) == 0);
    REQUIRE(::stat(target.c_str(), &changed) == 0);
    ctime_changed =
        changed.st_ctim.tv_sec != before.st_ctim.tv_sec || changed.st_ctim.tv_nsec != before.st_ctim.tv_nsec;
  }
  REQUIRE(ctime_changed);
  REQUIRE(changed.st_size == before.st_size);
  REQUIRE(changed.st_mtim.tv_sec == before.st_mtim.tv_sec);
  REQUIRE(changed.st_mtim.tv_nsec == before.st_mtim.tv_nsec);

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto written = co_await io::write_text_file(context.get_executor(),
                                                std::move(*mutation),
                                                "agent-v",
                                                io::WriteTextOptions{.atomic = true});
    REQUIRE_FALSE(written.has_value());
    REQUIRE(written.error().kind() == core::ErrorKind::conflict);
  });

  std::ifstream current{target, std::ios::binary};
  REQUIRE(std::string{std::istreambuf_iterator<char>{current}, std::istreambuf_iterator<char>{}} == "external");
}

TEST_CASE("FileMutation commit verifier aborts the atomic replacement and cleans its temp",
          "[unit][io][authority][mutation][atomic][conflict]") {
  TempDir temp{"oran-io-mutation-verifier-abort"};
  const auto workspace = temp.path() / "workspace";
  const auto target = workspace / "note.txt";
  std::filesystem::create_directories(workspace);
  write_direct(target, "original");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_file_mutation("note.txt");
  REQUIRE(mutation.has_value());

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    io::WriteTextOptions options{.atomic = true};
    options.verify_before_commit = [](const io::FileFingerprint& fp) -> core::Result<void> {
      REQUIRE(fp.size_bytes == 8);
      REQUIRE(fp.mtime_ns > 0);
      return std::unexpected(core::Error{core::ErrorKind::conflict, "verifier rejected the commit"});
    };
    auto written =
        co_await io::write_text_file(context.get_executor(), std::move(*mutation), "agent-version", std::move(options));
    REQUIRE_FALSE(written.has_value());
    REQUIRE(written.error().kind() == core::ErrorKind::conflict);
    REQUIRE(std::string_view{written.error().message()} == "verifier rejected the commit");
  });

  std::ifstream current{target, std::ios::binary};
  REQUIRE(std::string{std::istreambuf_iterator<char>{current}, std::istreambuf_iterator<char>{}} == "original");
  for (const auto& entry : std::filesystem::directory_iterator{workspace}) {
    REQUIRE(entry.path().filename().string().find(".orangutan.tmp.") == std::string::npos);
  }
}

TEST_CASE("FileMutation commit verifier observes the current target fingerprint",
          "[unit][io][authority][mutation][atomic]") {
  TempDir temp{"oran-io-mutation-verifier-ok"};
  const auto workspace = temp.path() / "workspace";
  const auto target = workspace / "note.txt";
  std::filesystem::create_directories(workspace);
  write_direct(target, "original");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_file_mutation("note.txt");
  REQUIRE(mutation.has_value());

  std::optional<io::FileFingerprint> observed;
  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    io::WriteTextOptions options{.atomic = true};
    options.verify_before_commit = [&observed](const io::FileFingerprint& fp) -> core::Result<void> {
      observed = fp;
      return {};
    };
    auto written =
        co_await io::write_text_file(context.get_executor(), std::move(*mutation), "agent-version", std::move(options));
    REQUIRE(written.has_value());
  });

  REQUIRE(observed.has_value());
  REQUIRE(observed->size_bytes == 8);
  REQUIRE(observed->mtime_ns > 0);
  std::ifstream current{target, std::ios::binary};
  REQUIRE(std::string{std::istreambuf_iterator<char>{current}, std::istreambuf_iterator<char>{}} == "agent-version");
}

TEST_CASE("FileMutation rejects commit verification outside atomic truncate mode",
          "[unit][io][authority][mutation][atomic]") {
  TempDir temp{"oran-io-mutation-verifier-mode"};
  const auto workspace = temp.path() / "workspace";
  std::filesystem::create_directories(workspace);
  write_direct(workspace / "note.txt", "original");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_file_mutation("note.txt");
  REQUIRE(mutation.has_value());

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    io::WriteTextOptions options{};
    options.verify_before_commit = [](const io::FileFingerprint&) -> core::Result<void> {
      return {};
    };
    auto written =
        co_await io::write_text_file(context.get_executor(), std::move(*mutation), "agent-version", std::move(options));
    REQUIRE_FALSE(written.has_value());
    REQUIRE(written.error().kind() == core::ErrorKind::invalid_argument);
  });

  std::ifstream current{workspace / "note.txt", std::ios::binary};
  REQUIRE(std::string{std::istreambuf_iterator<char>{current}, std::istreambuf_iterator<char>{}} == "original");
}

TEST_CASE("FileMutation can replace a writable target without read permission",
          "[unit][io][authority][mutation][permissions]") {
  TempDir temp{"oran-io-mutation-write-only"};
  const auto workspace = temp.path() / "workspace";
  const auto target = workspace / "note.txt";
  std::filesystem::create_directories(workspace);
  write_direct(target, "old");
  std::filesystem::permissions(target, std::filesystem::perms::owner_write, std::filesystem::perm_options::replace);

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_file_mutation("note.txt");
  REQUIRE(mutation.has_value());

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto written = co_await io::write_text_file(context.get_executor(),
                                                std::move(*mutation),
                                                "new",
                                                io::WriteTextOptions{.atomic = true});
    REQUIRE(written.has_value());
  });

  std::ifstream current{target, std::ios::binary};
  REQUIRE(std::string{std::istreambuf_iterator<char>{current}, std::istreambuf_iterator<char>{}} == "new");
}

TEST_CASE("FileMutation temp naming supports near-limit target names", "[unit][io][authority][mutation][name-max]") {
  TempDir temp{"oran-io-mutation-long-name"};
  const auto workspace = temp.path() / "workspace";
  std::filesystem::create_directories(workspace);
  const auto name = std::string(240U, 'n');

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_file_mutation(name);
  REQUIRE(mutation.has_value());

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto written = co_await io::write_text_file(context.get_executor(),
                                                std::move(*mutation),
                                                "content",
                                                io::WriteTextOptions{.atomic = true});
    REQUIRE(written.has_value());
  });

  REQUIRE(std::filesystem::file_size(workspace / name) == 7U);
}

namespace {

struct CollectedWalkEntry {
  std::string relative_path;
  io::DirectoryEntryKind kind{io::DirectoryEntryKind::other};
  std::size_t depth{0};
};

[[nodiscard]] io::WalkVisitor collect_into(std::vector<CollectedWalkEntry>& sink) {
  return [&sink](const io::DirectoryAuthority& /*parent*/, const io::WalkEntry& entry) -> core::Result<io::WalkAction> {
    sink.push_back(CollectedWalkEntry{
        .relative_path = entry.relative_path,
        .kind = entry.kind,
        .depth = entry.depth,
    });
    return io::WalkAction::proceed;
  };
}

[[nodiscard]] std::string_view context_reason(const core::Error& error) {
  const auto entries = error.context();
  const auto found = std::ranges::find_if(entries, [](const auto& entry) { return entry.first == "reason"; });
  return found == entries.end() ? std::string_view{} : std::string_view{found->second};
}

}  // namespace

TEST_CASE("walk_directory_tree visits a nested tree depth-first in sorted order", "[unit][io][authority][walk]") {
  TempDir temp{"oran-io-walk-nested"};
  const auto workspace = temp.path() / "workspace";
  std::filesystem::create_directories(workspace / "a" / "deep");
  std::filesystem::create_directories(workspace / "b");
  write_direct(workspace / "a" / "deep" / "leaf.txt", "leaf");
  write_direct(workspace / "a" / "note.txt", "note");
  write_direct(workspace / "b" / "b.txt", "bb");
  write_direct(workspace / "root.txt", "root");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());

  std::vector<CollectedWalkEntry> entries;
  auto visitor = collect_into(entries);
  auto walked = io::walk_directory_tree(*authority, io::WalkTreeOptions{}, [] { return false; }, visitor);
  REQUIRE(walked.has_value());

  REQUIRE(entries.size() == 7);
  CHECK(entries[0].relative_path == "a");
  CHECK(entries[0].kind == io::DirectoryEntryKind::directory);
  CHECK(entries[0].depth == 1);
  CHECK(entries[1].relative_path == "a/deep");
  CHECK(entries[1].depth == 2);
  CHECK(entries[2].relative_path == "a/deep/leaf.txt");
  CHECK(entries[2].depth == 3);
  CHECK(entries[3].relative_path == "a/note.txt");
  CHECK(entries[4].relative_path == "b");
  CHECK(entries[5].relative_path == "b/b.txt");
  CHECK(entries[6].relative_path == "root.txt");
}

TEST_CASE("walk_directory_tree classifies but never follows a symlinked directory", "[unit][io][authority][walk]") {
  TempDir temp{"oran-io-walk-symlink"};
  const auto workspace = temp.path() / "workspace";
  const auto outside = temp.path() / "outside";
  std::filesystem::create_directories(workspace);
  std::filesystem::create_directories(outside / "secret");
  write_direct(outside / "secret" / "leak.txt", "leak");
  std::filesystem::create_directory_symlink(outside, workspace / "escape");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());

  std::vector<CollectedWalkEntry> entries;
  auto visitor = collect_into(entries);
  auto walked = io::walk_directory_tree(*authority, io::WalkTreeOptions{}, [] { return false; }, visitor);
  REQUIRE(walked.has_value());

  REQUIRE(entries.size() == 1);
  CHECK(entries[0].relative_path == "escape");
  CHECK(entries[0].kind == io::DirectoryEntryKind::symlink);
}

TEST_CASE("walk_directory_tree honors skip_subtree without descending", "[unit][io][authority][walk]") {
  TempDir temp{"oran-io-walk-skip"};
  const auto workspace = temp.path() / "workspace";
  std::filesystem::create_directories(workspace / "skip" / "child");
  std::filesystem::create_directories(workspace / "keep");
  write_direct(workspace / "skip" / "child" / "hidden.txt", "x");
  write_direct(workspace / "keep" / "seen.txt", "y");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());

  std::vector<std::string> visited;
  io::WalkVisitor visitor = [&visited](const io::DirectoryAuthority& /*parent*/,
                                       const io::WalkEntry& entry) -> core::Result<io::WalkAction> {
    visited.push_back(entry.relative_path);
    if (entry.relative_path == "skip") {
      return io::WalkAction::skip_subtree;
    }
    return io::WalkAction::proceed;
  };
  auto walked = io::walk_directory_tree(*authority, io::WalkTreeOptions{}, [] { return false; }, visitor);
  REQUIRE(walked.has_value());

  REQUIRE(visited.size() == 3);
  CHECK(visited[0] == "keep");
  CHECK(visited[1] == "keep/seen.txt");
  CHECK(visited[2] == "skip");
  CHECK(std::ranges::find(visited, std::string{"skip/child"}) == visited.end());
}

TEST_CASE("walk_directory_tree stops immediately on WalkAction::stop", "[unit][io][authority][walk]") {
  TempDir temp{"oran-io-walk-stop"};
  const auto workspace = temp.path() / "workspace";
  std::filesystem::create_directories(workspace / "a");
  write_direct(workspace / "a" / "first.txt", "1");
  write_direct(workspace / "b.txt", "2");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());

  std::size_t seen = 0;
  io::WalkVisitor visitor = [&seen](const io::DirectoryAuthority& /*parent*/,
                                    const io::WalkEntry& /*entry*/) -> core::Result<io::WalkAction> {
    ++seen;
    return io::WalkAction::stop;
  };
  auto walked = io::walk_directory_tree(*authority, io::WalkTreeOptions{}, [] { return false; }, visitor);
  REQUIRE(walked.has_value());
  CHECK(seen == 1);
}

TEST_CASE("walk_directory_tree enforces the entry limit", "[unit][io][authority][walk]") {
  TempDir temp{"oran-io-walk-limit"};
  const auto workspace = temp.path() / "workspace";
  std::filesystem::create_directories(workspace);
  for (int i = 0; i < 5; ++i) {
    write_direct(workspace / std::format("f{}.txt", i), "x");
  }

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());

  std::vector<CollectedWalkEntry> entries;
  auto visitor = collect_into(entries);
  auto walked =
      io::walk_directory_tree(*authority, io::WalkTreeOptions{.max_entries = 3}, [] { return false; }, visitor);
  REQUIRE_FALSE(walked.has_value());
  CHECK(walked.error().kind() == core::ErrorKind::io);
  CHECK(context_reason(walked.error()) == "walk_entry_limit");
}

TEST_CASE("walk_directory_tree aborts when the cancel predicate fires", "[unit][io][authority][walk]") {
  TempDir temp{"oran-io-walk-cancel"};
  const auto workspace = temp.path() / "workspace";
  std::filesystem::create_directories(workspace);
  write_direct(workspace / "a.txt", "x");
  write_direct(workspace / "b.txt", "y");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());

  std::vector<CollectedWalkEntry> entries;
  auto visitor = collect_into(entries);
  auto walked = io::walk_directory_tree(*authority, io::WalkTreeOptions{}, [] { return true; }, visitor);
  REQUIRE_FALSE(walked.has_value());
  CHECK(walked.error().kind() == core::ErrorKind::cancelled);
  CHECK(entries.empty());
}

TEST_CASE("walk_directory_tree surfaces a visitor error", "[unit][io][authority][walk]") {
  TempDir temp{"oran-io-walk-visitor-error"};
  const auto workspace = temp.path() / "workspace";
  std::filesystem::create_directories(workspace);
  write_direct(workspace / "a.txt", "x");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());

  io::WalkVisitor visitor = [](const io::DirectoryAuthority& /*parent*/,
                               const io::WalkEntry& /*entry*/) -> core::Result<io::WalkAction> {
    return std::unexpected(core::Error::invalid_argument("visitor rejected entry"));
  };
  auto walked = io::walk_directory_tree(*authority, io::WalkTreeOptions{}, [] { return false; }, visitor);
  REQUIRE_FALSE(walked.has_value());
  CHECK(walked.error().kind() == core::ErrorKind::invalid_argument);
}

TEST_CASE("walk_directory_tree opens a matched file through the pinned parent authority",
          "[unit][io][authority][walk]") {
  TempDir temp{"oran-io-walk-open"};
  const auto workspace = temp.path() / "workspace";
  std::filesystem::create_directories(workspace / "sub");
  write_direct(workspace / "sub" / "leaf.txt", "anchored-content");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());

  std::string opened_text;
  io::WalkVisitor visitor = [&opened_text](const io::DirectoryAuthority& parent,
                                           const io::WalkEntry& entry) -> core::Result<io::WalkAction> {
    if (entry.kind == io::DirectoryEntryKind::regular_file) {
      auto file = parent.open_file(io::AnchoredPath{
          .relative_path = entry.name,
          .symlink_policy = io::AnchoredSymlinkPolicy::reject_all,
      });
      if (!file) {
        return std::unexpected(std::move(file).error());
      }
      opened_text = read_handle(*file);
    }
    return io::WalkAction::proceed;
  };
  auto walked = io::walk_directory_tree(*authority, io::WalkTreeOptions{}, [] { return false; }, visitor);
  REQUIRE(walked.has_value());
  CHECK(opened_text == "anchored-content");
}

TEST_CASE("walk_directory_tree prunes unreadable subtrees only on opt-in", "[unit][io][authority][walk]") {
  if (::geteuid() == 0) {
    SKIP("permission bits do not bind the superuser");
  }
  TempDir temp{"oran-io-walk-perms"};
  const auto workspace = temp.path() / "workspace";
  std::filesystem::create_directories(workspace / "locked");
  std::filesystem::create_directories(workspace / "open");
  write_direct(workspace / "locked" / "hidden.txt", "hidden");
  write_direct(workspace / "open" / "seen.txt", "seen");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());

  std::filesystem::permissions(workspace / "locked",
                               std::filesystem::perms::none,
                               std::filesystem::perm_options::replace);
  struct RestorePermissions {
    std::filesystem::path locked;
    ~RestorePermissions() {
      std::error_code ec;
      std::filesystem::permissions(locked,
                                   std::filesystem::perms::owner_all,
                                   std::filesystem::perm_options::replace,
                                   ec);
    }
  } restore{workspace / "locked"};

  std::vector<CollectedWalkEntry> strict_entries;
  auto strict_visitor = collect_into(strict_entries);
  auto strict = io::walk_directory_tree(*authority, io::WalkTreeOptions{}, [] { return false; }, strict_visitor);
  REQUIRE_FALSE(strict.has_value());
  CHECK(strict.error().kind() == core::ErrorKind::permission_denied);

  std::vector<CollectedWalkEntry> pruned_entries;
  auto pruned_visitor = collect_into(pruned_entries);
  auto pruned = io::walk_directory_tree(
      *authority,
      io::WalkTreeOptions{.max_entries = 0, .skip_permission_denied = true},
      [] { return false; },
      pruned_visitor);
  REQUIRE(pruned.has_value());

  REQUIRE(pruned_entries.size() == 3);
  CHECK(pruned_entries[0].relative_path == "locked");
  CHECK(pruned_entries[0].kind == io::DirectoryEntryKind::directory);
  CHECK(pruned_entries[1].relative_path == "open");
  CHECK(pruned_entries[2].relative_path == "open/seen.txt");
}
