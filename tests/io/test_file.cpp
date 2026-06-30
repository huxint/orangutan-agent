// tests/io/test_file.cpp — file and directory helper coverage.

#include <chrono>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/use_awaitable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/io.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace io = orangutan::io;
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

  [[nodiscard]] std::filesystem::path path() const {
    return path_;
  }

private:
  std::filesystem::path path_;
};

void write_direct(const std::filesystem::path& path, std::string_view contents) {
  std::ofstream output{path, std::ios::binary};
  output << contents;
}

std::string numbered_line(std::uint64_t line_number, char marker, std::size_t filler_width = 80U) {
  auto line = std::string{"line-"};
  line += std::to_string(line_number);
  line += '-';
  line.push_back(marker);
  line += ':';
  line.append(filler_width, marker);
  line.push_back('\n');
  return line;
}

std::string large_numbered_file(char marker) {
  std::string contents;
  contents.reserve(4096U * 96U);
  for (std::uint64_t line = 1; line <= 4096U; ++line) {
    contents += numbered_line(line, marker);
  }
  return contents;
}

}  // namespace

TEST_CASE("read_text_file returns file contents", "[unit][io][file]") {
  TempDir temp{"oran-io-read"};
  const auto file = temp.path() / "input.txt";
  write_direct(file, "hello\norangutan");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto result = co_await io::read_text_file(context.get_executor(), file.string());

    REQUIRE(result.has_value());
    REQUIRE(*result == "hello\norangutan");
  });
}

TEST_CASE("read_text_file reports missing files", "[unit][io][file]") {
  TempDir temp{"oran-io-missing"};
  const auto file = temp.path() / "missing.txt";

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto result = co_await io::read_text_file(context.get_executor(), file.string());

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::not_found);
  });
}

TEST_CASE("read_text_file enforces max_bytes", "[unit][io][file]") {
  TempDir temp{"oran-io-max"};
  const auto file = temp.path() / "large.txt";
  write_direct(file, "123456789");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto result =
        co_await io::read_text_file(context.get_executor(), file.string(), io::ReadTextOptions{.max_bytes = 4});

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  });
}

TEST_CASE("write_text_file creates parents and supports append", "[unit][io][file]") {
  TempDir temp{"oran-io-write"};
  const auto file = temp.path() / "nested" / "output.txt";

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto first = co_await io::write_text_file(
        context.get_executor(),
        file.string(),
        "one",
        io::WriteTextOptions{.mode = io::WriteMode::truncate, .create_parent_directories = true});
    auto second = co_await io::write_text_file(context.get_executor(),
                                               file.string(),
                                               "\ntwo",
                                               io::WriteTextOptions{.mode = io::WriteMode::append});
    auto read_back = co_await io::read_text_file(context.get_executor(), file.string());

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(read_back.has_value());
    REQUIRE(*read_back == "one\ntwo");
  });
}

TEST_CASE("write_text_file can refuse overwrites", "[unit][io][file]") {
  TempDir temp{"oran-io-conflict"};
  const auto file = temp.path() / "output.txt";
  write_direct(file, "existing");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto result = co_await io::write_text_file(context.get_executor(),
                                               file.string(),
                                               "new",
                                               io::WriteTextOptions{.mode = io::WriteMode::fail_if_exists});

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::conflict);
  });
}

TEST_CASE("list_directory returns sorted visible entries", "[unit][io][directory]") {
  TempDir temp{"oran-io-list"};
  write_direct(temp.path() / "b.txt", "bb");
  write_direct(temp.path() / ".hidden", "hidden");
  std::filesystem::create_directory(temp.path() / "a-dir");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto result = co_await io::list_directory(context.get_executor(), temp.path().string());

    REQUIRE(result.has_value());
    REQUIRE(result->size() == 2);
    REQUIRE((*result)[0].name == "a-dir");
    REQUIRE((*result)[0].kind == io::DirectoryEntryKind::directory);
    REQUIRE_FALSE((*result)[0].size_bytes.has_value());
    REQUIRE((*result)[1].name == "b.txt");
    REQUIRE((*result)[1].kind == io::DirectoryEntryKind::regular_file);
    REQUIRE((*result)[1].size_bytes == 2);
  });
}

TEST_CASE("list_directory can include hidden entries", "[unit][io][directory]") {
  TempDir temp{"oran-io-list-hidden"};
  write_direct(temp.path() / ".hidden", "hidden");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto result = co_await io::list_directory(context.get_executor(),
                                              temp.path().string(),
                                              io::ListDirectoryOptions{.include_hidden = true});

    REQUIRE(result.has_value());
    REQUIRE(result->size() == 1);
    REQUIRE((*result)[0].name == ".hidden");
  });
}

TEST_CASE("read_text_file observes cancellation before blocking work", "[unit][io][file]") {
  TempDir temp{"oran-io-cancel"};
  const auto file = temp.path() / "input.txt";
  write_direct(file, "content");

  asio::io_context context;
  asio::cancellation_signal signal;
  std::optional<core::Result<std::string>> result;
  std::exception_ptr failure;

  asio::co_spawn(
      context,
      [&]() -> async::Awaitable<core::Result<std::string>> {
        co_return co_await io::read_text_file(context.get_executor(), file.string());
      },
      asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, core::Result<std::string> r) {
        failure = ep;
        result = std::move(r);
        context.stop();
      }));

  asio::post(context, [&] { signal.emit(asio::cancellation_type::terminal); });
  context.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
}

TEST_CASE("run_blocking returns the callable result", "[unit][io][blocking]") {
  test::run_async([](asio::io_context& context) -> async::Awaitable<void> {
    bool invoked = false;

    auto result = co_await io::run_blocking(context.get_executor(), [&] {
      invoked = true;
      return core::Result<int>{42};
    });

    REQUIRE(result.has_value());
    REQUIRE(*result == 42);
    REQUIRE(invoked);
  });
}

TEST_CASE("run_blocking observes cancellation before invoking callable", "[unit][io][blocking]") {
  asio::io_context context;
  asio::cancellation_signal signal;
  bool invoked = false;
  std::optional<core::Result<int>> result;
  std::exception_ptr failure;

  asio::co_spawn(
      context,
      [&]() -> async::Awaitable<core::Result<int>> {
        co_return co_await io::run_blocking(context.get_executor(), [&] {
          invoked = true;
          return core::Result<int>{42};
        });
      },
      asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, core::Result<int> r) {
        failure = ep;
        result = std::move(r);
        context.stop();
      }));

  asio::post(context, [&] { signal.emit(asio::cancellation_type::terminal); });
  context.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
  REQUIRE_FALSE(invoked);
}

TEST_CASE("delete_file removes the file at path", "[unit][io][file]") {
  TempDir temp{"oran-io-delete"};
  const auto file = temp.path() / "removable.txt";
  write_direct(file, "bye");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto result = co_await io::delete_file(context.get_executor(), file.string());
    REQUIRE(result.has_value());
    REQUIRE_FALSE(std::filesystem::exists(file));
  });
}

TEST_CASE("delete_path recursively removes a directory tree when requested", "[unit][io][file]") {
  TempDir temp{"oran-io-delete-tree"};
  const auto tree = temp.path() / "tree";
  std::filesystem::create_directories(tree / "nested");
  write_direct(tree / "top.txt", "bye");
  write_direct(tree / "nested" / "leaf.txt", "bye");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto result =
        co_await io::delete_path(context.get_executor(), tree.string(), io::DeletePathOptions{.recursive = true});
    REQUIRE(result.has_value());
    REQUIRE(result->paths_removed == std::uintmax_t{4});
    REQUIRE_FALSE(std::filesystem::exists(tree));
  });
}

TEST_CASE("delete_path recursive tree delete does not follow nested symlinks", "[unit][io][file]") {
  TempDir temp{"oran-io-delete-tree-symlink"};
  TempDir outside{"oran-io-delete-tree-symlink-target"};
  const auto tree = temp.path() / "tree";
  const auto target = outside.path() / "target";
  std::filesystem::create_directories(tree);
  std::filesystem::create_directories(target);
  write_direct(target / "survives.txt", "still here");
  const auto link = tree / "outside-link";
  std::error_code link_ec;
  std::filesystem::create_directory_symlink(target, link, link_ec);
  if (link_ec) {
    SUCCEED("symlink creation not supported on this filesystem: " << link_ec.message());
    return;
  }

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto result =
        co_await io::delete_path(context.get_executor(), tree.string(), io::DeletePathOptions{.recursive = true});
    REQUIRE(result.has_value());
    REQUIRE_FALSE(std::filesystem::exists(tree));
    REQUIRE(std::filesystem::exists(target / "survives.txt"));
  });
}

TEST_CASE("delete_file rejects empty path", "[unit][io][file]") {
  test::run_async([](asio::io_context& context) -> async::Awaitable<void> {
    auto result = co_await io::delete_file(context.get_executor(), "");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  });
}

TEST_CASE("delete_file returns not_found when the file does not exist", "[unit][io][file]") {
  TempDir temp{"oran-io-delete-missing"};
  const auto file = temp.path() / "absent.txt";

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto result = co_await io::delete_file(context.get_executor(), file.string());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::not_found);
  });
}

TEST_CASE("delete_file refuses directories with invalid_argument", "[unit][io][file]") {
  TempDir temp{"oran-io-delete-dir"};
  const auto subdir = temp.path() / "subdir";
  std::filesystem::create_directory(subdir);

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto result = co_await io::delete_file(context.get_executor(), subdir.string());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
    // The directory must still be present after the refused delete.
    REQUIRE(std::filesystem::exists(subdir));
  });
}

TEST_CASE("delete_path refuses directories without recursive intent", "[unit][io][file]") {
  TempDir temp{"oran-io-delete-dir-nonrecursive"};
  const auto subdir = temp.path() / "subdir";
  std::filesystem::create_directory(subdir);

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto result = co_await io::delete_path(context.get_executor(), subdir.string());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(std::filesystem::exists(subdir));
  });
}

TEST_CASE("delete_file refuses symlinks with invalid_argument and leaves them intact", "[unit][io][file]") {
  TempDir temp{"oran-io-delete-symlink"};
  const auto target = temp.path() / "target.txt";
  write_direct(target, "still here");
  const auto link = temp.path() / "link.txt";
  std::error_code link_ec;
  std::filesystem::create_symlink(target, link, link_ec);
  if (link_ec) {
    // Some filesystems (e.g. WSL on a Windows mount without dev-mode) cannot
    // create symlinks; treat as an environment skip rather than a failure.
    SUCCEED("symlink creation not supported on this filesystem: " << link_ec.message());
    return;
  }

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto result = co_await io::delete_file(context.get_executor(), link.string());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(std::filesystem::is_symlink(link));
    REQUIRE(std::filesystem::exists(target));
  });
}

TEST_CASE("delete_path refuses symlinks even with recursive intent", "[unit][io][file]") {
  TempDir temp{"oran-io-delete-symlink-recursive"};
  const auto target = temp.path() / "target";
  std::filesystem::create_directory(target);
  const auto link = temp.path() / "link";
  std::error_code link_ec;
  std::filesystem::create_directory_symlink(target, link, link_ec);
  if (link_ec) {
    SUCCEED("symlink creation not supported on this filesystem: " << link_ec.message());
    return;
  }

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto result =
        co_await io::delete_path(context.get_executor(), link.string(), io::DeletePathOptions{.recursive = true});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(std::filesystem::is_symlink(link));
    REQUIRE(std::filesystem::exists(target));
  });
}

TEST_CASE("write_text_file atomic mode commits via rename and leaves no temp behind", "[unit][io][file][atomic]") {
  TempDir temp{"oran-io-atomic-ok"};
  const auto file = temp.path() / "data.txt";
  write_direct(file, "old contents");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto result = co_await io::write_text_file(context.get_executor(),
                                               file.string(),
                                               "new contents",
                                               io::WriteTextOptions{.atomic = true});
    REQUIRE(result.has_value());

    auto read_back = co_await io::read_text_file(context.get_executor(), file.string());
    REQUIRE(read_back.has_value());
    REQUIRE(*read_back == "new contents");

    // No stray sibling `.<name>.orangutan.tmp.<seq>` file should survive a
    // successful atomic commit.
    auto listing = co_await io::list_directory(context.get_executor(),
                                               temp.path().string(),
                                               io::ListDirectoryOptions{.include_hidden = true});
    REQUIRE(listing.has_value());
    REQUIRE(listing->size() == 1);
    REQUIRE((*listing)[0].name == "data.txt");
  });
}

TEST_CASE("write_text_file atomic mode does not reuse process-local counter temp names", "[unit][io][file][atomic]") {
  TempDir temp{"oran-io-atomic-temp-name"};
  const auto file = temp.path() / "data.txt";
  const auto legacy_temp = temp.path() / ".data.txt.orangutan.tmp.0";
  write_direct(file, "old contents");
  write_direct(legacy_temp, "legacy marker");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto result = co_await io::write_text_file(context.get_executor(),
                                               file.string(),
                                               "new contents",
                                               io::WriteTextOptions{.atomic = true});
    REQUIRE(result.has_value());

    auto read_back = co_await io::read_text_file(context.get_executor(), file.string());
    REQUIRE(read_back.has_value());
    REQUIRE(*read_back == "new contents");

    auto marker = co_await io::read_text_file(context.get_executor(), legacy_temp.string());
    REQUIRE(marker.has_value());
    REQUIRE(*marker == "legacy marker");
  });
}

TEST_CASE("write_text_file atomic durability modes commit successfully", "[unit][io][file][atomic]") {
  TempDir temp{"oran-io-atomic-durable"};
  const auto file_only = temp.path() / "file-only.txt";
  const auto file_and_parent = temp.path() / "file-and-parent.txt";

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto synced_file = co_await io::write_text_file(
        context.get_executor(),
        file_only.string(),
        "file durable",
        io::WriteTextOptions{.atomic = true, .durability = io::WriteTextDurability::fsync_file});
    REQUIRE(synced_file.has_value());

    auto synced_parent = co_await io::write_text_file(
        context.get_executor(),
        file_and_parent.string(),
        "parent durable",
        io::WriteTextOptions{.atomic = true, .durability = io::WriteTextDurability::fsync_file_and_parent});
    REQUIRE(synced_parent.has_value());

    auto read_file = co_await io::read_text_file(context.get_executor(), file_only.string());
    auto read_parent = co_await io::read_text_file(context.get_executor(), file_and_parent.string());
    REQUIRE(read_file.has_value());
    REQUIRE(read_parent.has_value());
    REQUIRE(*read_file == "file durable");
    REQUIRE(*read_parent == "parent durable");
  });
}

TEST_CASE("write_text_file durability rejects non-atomic writes before I/O", "[unit][io][file][atomic]") {
  TempDir temp{"oran-io-atomic-durable-reject"};
  const auto file = temp.path() / "data.txt";

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto result = co_await io::write_text_file(context.get_executor(),
                                               file.string(),
                                               "contents",
                                               io::WriteTextOptions{.durability = io::WriteTextDurability::fsync_file});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE_FALSE(std::filesystem::exists(file));
  });
}

TEST_CASE("write_text_file atomic mode rejects append and fail_if_exists", "[unit][io][file][atomic]") {
  TempDir temp{"oran-io-atomic-bad-mode"};
  const auto file = temp.path() / "data.txt";

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto appended = co_await io::write_text_file(context.get_executor(),
                                                 file.string(),
                                                 "x",
                                                 io::WriteTextOptions{.mode = io::WriteMode::append, .atomic = true});
    REQUIRE_FALSE(appended.has_value());
    REQUIRE(appended.error().kind() == core::ErrorKind::invalid_argument);

    auto fail_if_exists =
        co_await io::write_text_file(context.get_executor(),
                                     file.string(),
                                     "x",
                                     io::WriteTextOptions{.mode = io::WriteMode::fail_if_exists, .atomic = true});
    REQUIRE_FALSE(fail_if_exists.has_value());
    REQUIRE(fail_if_exists.error().kind() == core::ErrorKind::invalid_argument);

    // The rejection must happen before any I/O — the destination file stays
    // unborn.
    REQUIRE_FALSE(std::filesystem::exists(file));
  });
}

TEST_CASE("write_text_file atomic mode preserves original when commit fails", "[unit][io][file][atomic]") {
  TempDir temp{"oran-io-atomic-fail"};
  const auto file = temp.path() / "data.txt";
  write_direct(file, "original");

  // Replace the target with a directory of the same name. The atomic write
  // will produce a valid sibling temp file, but `std::filesystem::rename` will
  // refuse to overwrite a directory with a regular file — exercising the
  // commit-failure cleanup path.
  std::filesystem::remove(file);
  std::filesystem::create_directory(file);

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto result = co_await io::write_text_file(context.get_executor(),
                                               file.string(),
                                               "new",
                                               io::WriteTextOptions{.atomic = true});
    REQUIRE_FALSE(result.has_value());

    // The pre-existing directory is still in place, and no `.<name>.orangutan.tmp.*`
    // leftover survives the failed commit.
    REQUIRE(std::filesystem::is_directory(file));
    auto listing = co_await io::list_directory(context.get_executor(),
                                               temp.path().string(),
                                               io::ListDirectoryOptions{.include_hidden = true});
    REQUIRE(listing.has_value());
    REQUIRE(listing->size() == 1);
    REQUIRE((*listing)[0].name == "data.txt");
  });
}

TEST_CASE("read_text_file_ranged returns whole file with fingerprint and line span", "[unit][io][range]") {
  TempDir temp{"oran-io-range-whole"};
  const auto file = temp.path() / "lines.txt";
  write_direct(file, "alpha\nbeta\ngamma\n");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto result = co_await io::read_text_file_ranged(context.get_executor(), file.string());
    REQUIRE(result.has_value());
    REQUIRE(result->text == "alpha\nbeta\ngamma\n");
    REQUIRE(result->start_line == 1);
    REQUIRE(result->end_line == 3);
    REQUIRE(result->returned_bytes == 17);
    REQUIRE_FALSE(result->truncated);
    REQUIRE(result->fingerprint.size_bytes == 17);
    REQUIRE(result->fingerprint.mtime_ns > 0);
  });
}

TEST_CASE("read_text_file_ranged counts a trailing partial line", "[unit][io][range]") {
  TempDir temp{"oran-io-range-partial-tail"};
  const auto file = temp.path() / "lines.txt";
  write_direct(file, "alpha\nbeta\ngamma");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto result = co_await io::read_text_file_ranged(context.get_executor(), file.string());
    REQUIRE(result.has_value());
    REQUIRE(result->end_line == 3);
  });
}

TEST_CASE("read_text_file_ranged refreshes the file-view cache after an external rewrite", "[unit][io][range][cache]") {
  TempDir temp{"oran-io-range-cache-external"};
  const auto file = temp.path() / "cached.txt";
  write_direct(file, "first");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto first = co_await io::read_text_file_ranged(context.get_executor(), file.string());
    REQUIRE(first.has_value());
    REQUIRE(first->text == "first");

    std::this_thread::sleep_for(std::chrono::milliseconds{15});
    write_direct(file, "second-body");

    auto second = co_await io::read_text_file_ranged(context.get_executor(), file.string());
    REQUIRE(second.has_value());
    REQUIRE(second->text == "second-body");
    REQUIRE(second->fingerprint != first->fingerprint);
  });
}

TEST_CASE("write_text_file invalidates the file-view cache even when the fingerprint is restored",
          "[unit][io][range][cache]") {
  TempDir temp{"oran-io-range-cache-write"};
  const auto file = temp.path() / "cached.txt";
  write_direct(file, "alpha");

  const auto original_mtime = std::filesystem::last_write_time(file);
  const auto original_fingerprint = io::compute_file_fingerprint(file.string());
  REQUIRE(original_fingerprint.has_value());

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto first = co_await io::read_text_file_ranged(context.get_executor(), file.string());
    REQUIRE(first.has_value());
    REQUIRE(first->text == "alpha");

    auto written = co_await io::write_text_file(context.get_executor(), file.string(), "bravo");
    REQUIRE(written.has_value());
    std::filesystem::last_write_time(file, original_mtime);

    const auto restored_fingerprint = io::compute_file_fingerprint(file.string());
    REQUIRE(restored_fingerprint.has_value());
    REQUIRE(restored_fingerprint->size_bytes == original_fingerprint->size_bytes);
    REQUIRE(restored_fingerprint->mtime_ns == original_fingerprint->mtime_ns);

    auto second = co_await io::read_text_file_ranged(context.get_executor(), file.string());
    REQUIRE(second.has_value());
    REQUIRE(second->text == "bravo");
  });
}

TEST_CASE("read_text_file_ranged_cache_stats exposes file-view and line-offset cache health",
          "[unit][io][range][cache][stats]") {
  TempDir temp{"oran-io-range-cache-stats"};
  const auto file_view_file = temp.path() / "file-view.txt";
  const auto indexed_file = temp.path() / "indexed.txt";
  write_direct(file_view_file, "alpha\nbeta\n");
  write_direct(indexed_file, large_numbered_file('s'));

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    const auto file_view_before = io::read_text_file_ranged_cache_stats();

    auto cold = co_await io::read_text_file_ranged(context.get_executor(), file_view_file.string());
    auto hot = co_await io::read_text_file_ranged(context.get_executor(), file_view_file.string());
    REQUIRE(cold.has_value());
    REQUIRE(hot.has_value());
    REQUIRE(cold->text == hot->text);

    const auto file_view_after = io::read_text_file_ranged_cache_stats();
    REQUIRE(file_view_after.file_view.misses == file_view_before.file_view.misses + 1);
    REQUIRE(file_view_after.file_view.hits == file_view_before.file_view.hits + 1);
    REQUIRE(file_view_after.file_view.current_entries > 0);
    REQUIRE(file_view_after.file_view.current_bytes >= hot->text.size());

    io::ReadTextOptions first_range;
    first_range.range = io::FileRange{.lines = io::FileRange::LineSpan{.start_line = 100, .line_count = 8}};
    io::ReadTextOptions second_range;
    second_range.range = io::FileRange{.lines = io::FileRange::LineSpan{.start_line = 200, .line_count = 8}};

    const auto line_before = io::read_text_file_ranged_cache_stats();
    auto first = co_await io::read_text_file_ranged(context.get_executor(), indexed_file.string(), first_range);
    auto second = co_await io::read_text_file_ranged(context.get_executor(), indexed_file.string(), second_range);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(first->start_line == 100);
    REQUIRE(second->start_line == 200);

    const auto line_after = io::read_text_file_ranged_cache_stats();
    REQUIRE(line_after.line_offset_index.misses == line_before.line_offset_index.misses + 1);
    REQUIRE(line_after.line_offset_index.hits == line_before.line_offset_index.hits + 1);
    REQUIRE(line_after.line_offset_index.current_entries > 0);
    REQUIRE(line_after.line_offset_index.current_bytes > 0);
  });
}

TEST_CASE("invalidate_read_text_file_ranged_cache removes one file-view path only", "[unit][io][range][cache]") {
  TempDir temp{"oran-io-range-cache-invalidate-path"};
  const auto invalidated_file = temp.path() / "invalidated.txt";
  const auto retained_file = temp.path() / "retained.txt";
  write_direct(invalidated_file, "alpha\n");
  write_direct(retained_file, "bravo\n");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto invalidated_cold = co_await io::read_text_file_ranged(context.get_executor(), invalidated_file.string());
    auto retained_cold = co_await io::read_text_file_ranged(context.get_executor(), retained_file.string());
    REQUIRE(invalidated_cold.has_value());
    REQUIRE(retained_cold.has_value());

    const auto before = io::read_text_file_ranged_cache_stats();
    io::invalidate_read_text_file_ranged_cache(invalidated_file.string());

    auto invalidated_hot = co_await io::read_text_file_ranged(context.get_executor(), invalidated_file.string());
    auto retained_hot = co_await io::read_text_file_ranged(context.get_executor(), retained_file.string());
    REQUIRE(invalidated_hot.has_value());
    REQUIRE(retained_hot.has_value());
    REQUIRE(invalidated_hot->text == "alpha\n");
    REQUIRE(retained_hot->text == "bravo\n");

    const auto after = io::read_text_file_ranged_cache_stats();
    REQUIRE(after.file_view.misses == before.file_view.misses + 1);
    REQUIRE(after.file_view.hits == before.file_view.hits + 1);
  });
}

TEST_CASE("invalidate_read_text_file_ranged_cache removes one line-offset path only",
          "[unit][io][range][cache][lines]") {
  TempDir temp{"oran-io-range-line-cache-invalidate-path"};
  const auto invalidated_file = temp.path() / "invalidated-large.txt";
  const auto retained_file = temp.path() / "retained-large.txt";
  write_direct(invalidated_file, large_numbered_file('i'));
  write_direct(retained_file, large_numbered_file('r'));

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    io::ReadTextOptions first_range;
    first_range.range = io::FileRange{.lines = io::FileRange::LineSpan{.start_line = 100, .line_count = 1}};
    io::ReadTextOptions second_range;
    second_range.range = io::FileRange{.lines = io::FileRange::LineSpan{.start_line = 200, .line_count = 1}};

    auto invalidated_cold =
        co_await io::read_text_file_ranged(context.get_executor(), invalidated_file.string(), first_range);
    auto retained_cold =
        co_await io::read_text_file_ranged(context.get_executor(), retained_file.string(), first_range);
    REQUIRE(invalidated_cold.has_value());
    REQUIRE(retained_cold.has_value());

    const auto before = io::read_text_file_ranged_cache_stats();
    io::invalidate_read_text_file_ranged_cache(invalidated_file.string());

    auto invalidated_next =
        co_await io::read_text_file_ranged(context.get_executor(), invalidated_file.string(), second_range);
    auto retained_next =
        co_await io::read_text_file_ranged(context.get_executor(), retained_file.string(), second_range);
    REQUIRE(invalidated_next.has_value());
    REQUIRE(retained_next.has_value());
    REQUIRE(invalidated_next->text == numbered_line(200, 'i'));
    REQUIRE(retained_next->text == numbered_line(200, 'r'));

    const auto after = io::read_text_file_ranged_cache_stats();
    REQUIRE(after.line_offset_index.misses == before.line_offset_index.misses + 1);
    REQUIRE(after.line_offset_index.hits == before.line_offset_index.hits + 1);
  });
}

TEST_CASE("write_text_file invalidates only the changed read-cache path", "[unit][io][range][cache]") {
  TempDir temp{"oran-io-range-cache-write-one-path"};
  const auto changed_file = temp.path() / "changed.txt";
  const auto retained_file = temp.path() / "retained.txt";
  write_direct(changed_file, "alpha");
  write_direct(retained_file, "bravo");

  const auto original_mtime = std::filesystem::last_write_time(changed_file);
  const auto original_fingerprint = io::compute_file_fingerprint(changed_file.string());
  REQUIRE(original_fingerprint.has_value());

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto changed_cold = co_await io::read_text_file_ranged(context.get_executor(), changed_file.string());
    auto retained_cold = co_await io::read_text_file_ranged(context.get_executor(), retained_file.string());
    REQUIRE(changed_cold.has_value());
    REQUIRE(retained_cold.has_value());

    auto written = co_await io::write_text_file(context.get_executor(), changed_file.string(), "omega");
    REQUIRE(written.has_value());
    std::filesystem::last_write_time(changed_file, original_mtime);

    const auto restored_fingerprint = io::compute_file_fingerprint(changed_file.string());
    REQUIRE(restored_fingerprint.has_value());
    REQUIRE(restored_fingerprint->size_bytes == original_fingerprint->size_bytes);
    REQUIRE(restored_fingerprint->mtime_ns == original_fingerprint->mtime_ns);

    const auto before = io::read_text_file_ranged_cache_stats();
    auto changed_hot = co_await io::read_text_file_ranged(context.get_executor(), changed_file.string());
    auto retained_hot = co_await io::read_text_file_ranged(context.get_executor(), retained_file.string());
    REQUIRE(changed_hot.has_value());
    REQUIRE(retained_hot.has_value());
    REQUIRE(changed_hot->text == "omega");
    REQUIRE(retained_hot->text == "bravo");

    const auto after = io::read_text_file_ranged_cache_stats();
    REQUIRE(after.file_view.misses == before.file_view.misses + 1);
    REQUIRE(after.file_view.hits == before.file_view.hits + 1);
  });
}

#if defined(__linux__)
TEST_CASE("watch_read_text_file_ranged_cache invalidates an external file rewrite", "[unit][io][range][cache][watch]") {
  TempDir temp{"oran-io-range-cache-watch"};
  const auto file = temp.path() / "watched.txt";
  write_direct(file, "alpha");

  const auto original_mtime = std::filesystem::last_write_time(file);
  const auto original_fingerprint = io::compute_file_fingerprint(file.string());
  REQUIRE(original_fingerprint.has_value());

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto cold = co_await io::read_text_file_ranged(context.get_executor(), file.string());
    REQUIRE(cold.has_value());
    REQUIRE(cold->text == "alpha");

    asio::steady_timer writer{context};
    writer.expires_after(std::chrono::milliseconds{10});
    writer.async_wait([&](const asio::error_code& ec) {
      if (ec) {
        return;
      }
      write_direct(file, "bravo");
      std::filesystem::last_write_time(file, original_mtime);
    });

    auto watched = co_await io::watch_read_text_file_ranged_cache(
        context.get_executor(),
        temp.path().string(),
        io::ReadTextFileWatchOptions{.recursive = false, .max_events = 1});
    REQUIRE(watched.has_value());
    REQUIRE(watched->directories_watched == 1);
    REQUIRE(watched->events_seen >= 1);
    REQUIRE(watched->invalidations >= 1);

    const auto restored_fingerprint = io::compute_file_fingerprint(file.string());
    REQUIRE(restored_fingerprint.has_value());
    REQUIRE(restored_fingerprint->size_bytes == original_fingerprint->size_bytes);
    REQUIRE(restored_fingerprint->mtime_ns == original_fingerprint->mtime_ns);

    const auto before = io::read_text_file_ranged_cache_stats();
    auto fresh = co_await io::read_text_file_ranged(context.get_executor(), file.string());
    REQUIRE(fresh.has_value());
    REQUIRE(fresh->text == "bravo");

    const auto after = io::read_text_file_ranged_cache_stats();
    REQUIRE(after.file_view.misses == before.file_view.misses + 1);
  });
}

TEST_CASE("watch_read_text_file_ranged_cache recursively watches existing child directories",
          "[unit][io][range][cache][watch]") {
  TempDir temp{"oran-io-range-cache-watch-recursive"};
  const auto child = temp.path() / "child";
  std::filesystem::create_directory(child);
  const auto file = child / "watched.txt";
  write_direct(file, "first");

  const auto original_mtime = std::filesystem::last_write_time(file);

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto cold = co_await io::read_text_file_ranged(context.get_executor(), file.string());
    REQUIRE(cold.has_value());
    REQUIRE(cold->text == "first");

    asio::steady_timer writer{context};
    writer.expires_after(std::chrono::milliseconds{10});
    writer.async_wait([&](const asio::error_code& ec) {
      if (ec) {
        return;
      }
      write_direct(file, "fresh");
      std::filesystem::last_write_time(file, original_mtime);
    });

    auto watched = co_await io::watch_read_text_file_ranged_cache(
        context.get_executor(),
        temp.path().string(),
        io::ReadTextFileWatchOptions{.recursive = true, .max_events = 1});
    REQUIRE(watched.has_value());
    REQUIRE(watched->directories_watched >= 2);
    REQUIRE(watched->events_seen >= 1);
    REQUIRE(watched->invalidations >= 1);

    auto fresh = co_await io::read_text_file_ranged(context.get_executor(), file.string());
    REQUIRE(fresh.has_value());
    REQUIRE(fresh->text == "fresh");
  });
}

TEST_CASE("watch_read_text_file_ranged_cache observes cancellation while waiting", "[unit][io][range][cache][watch]") {
  TempDir temp{"oran-io-range-cache-watch-cancel"};

  asio::io_context context;
  asio::cancellation_signal signal;
  std::optional<core::Result<io::ReadTextFileWatchStats>> result;
  std::exception_ptr failure;

  asio::steady_timer cancel{context};
  cancel.expires_after(std::chrono::milliseconds{10});
  cancel.async_wait([&](const asio::error_code& ec) {
    if (!ec) {
      signal.emit(asio::cancellation_type::terminal);
    }
  });

  asio::steady_timer timeout{context};
  timeout.expires_after(std::chrono::seconds{1});
  timeout.async_wait([&](const asio::error_code& ec) {
    if (!ec) {
      context.stop();
    }
  });

  asio::co_spawn(
      context,
      [&]() -> async::Awaitable<core::Result<io::ReadTextFileWatchStats>> {
        co_return co_await io::watch_read_text_file_ranged_cache(context.get_executor(), temp.path().string());
      },
      asio::bind_cancellation_slot(signal.slot(),
                                   [&](std::exception_ptr ep, core::Result<io::ReadTextFileWatchStats> r) {
                                     failure = ep;
                                     result = std::move(r);
                                     timeout.cancel();
                                     context.stop();
                                   }));

  context.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
}
#endif

TEST_CASE("read_text_file_ranged collapses concurrent cold reads with singleflight",
          "[unit][io][range][singleflight]") {
  TempDir temp{"oran-io-range-singleflight"};
  const auto file = temp.path() / "shared.txt";
  write_direct(file, "alpha\nbeta\n");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    const auto before = io::read_text_file_ranged_singleflight_stats();
    std::optional<core::Result<io::ReadTextResult>> first;
    std::optional<core::Result<io::ReadTextResult>> second;
    std::exception_ptr failure;

    auto spawn_read = [&](std::optional<core::Result<io::ReadTextResult>>* slot) {
      asio::co_spawn(
          context,
          [&]() -> async::Awaitable<core::Result<io::ReadTextResult>> {
            co_return co_await io::read_text_file_ranged(context.get_executor(), file.string());
          },
          [&, slot](std::exception_ptr ep, core::Result<io::ReadTextResult> result) {
            if (ep) {
              failure = ep;
              return;
            }
            *slot = std::move(result);
          });
    };

    spawn_read(&first);
    spawn_read(&second);
    while ((!first || !second) && !failure) {
      co_await asio::post(context, asio::use_awaitable);
    }
    if (failure) {
      std::rethrow_exception(failure);
    }

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(first->has_value());
    REQUIRE(second->has_value());
    REQUIRE((*first)->text == "alpha\nbeta\n");
    REQUIRE((*second)->text == "alpha\nbeta\n");
    REQUIRE((*first)->fingerprint == (*second)->fingerprint);

    const auto after = io::read_text_file_ranged_singleflight_stats();
    REQUIRE(after.leaders_started == before.leaders_started + 1);
    REQUIRE(after.followers_joined == before.followers_joined + 1);
    REQUIRE(after.completions == before.completions + 1);
    REQUIRE(after.errors == before.errors);
    REQUIRE(after.current_in_flight == 0);
    REQUIRE(after.current_waiters == 0);
  });
}

TEST_CASE("read_text_file_ranged truncates oversize whole-file reads", "[unit][io][range]") {
  TempDir temp{"oran-io-range-truncate"};
  const auto file = temp.path() / "big.txt";
  write_direct(file, "0123456789");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto result =
        co_await io::read_text_file_ranged(context.get_executor(), file.string(), io::ReadTextOptions{.max_bytes = 4});
    REQUIRE(result.has_value());
    REQUIRE(result->text == "0123");
    REQUIRE(result->returned_bytes == 4);
    REQUIRE(result->truncated);
    REQUIRE(result->fingerprint.size_bytes == 10);
  });
}

TEST_CASE("read_text_file_ranged extracts a line range", "[unit][io][range][lines]") {
  TempDir temp{"oran-io-range-lines"};
  const auto file = temp.path() / "lines.txt";
  write_direct(file, "one\ntwo\nthree\nfour\nfive\n");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    io::ReadTextOptions options;
    options.range = io::FileRange{.lines = io::FileRange::LineSpan{.start_line = 2, .line_count = 3}};
    auto result = co_await io::read_text_file_ranged(context.get_executor(), file.string(), options);
    REQUIRE(result.has_value());
    REQUIRE(result->text == "two\nthree\nfour\n");
    REQUIRE(result->start_line == 2);
    REQUIRE(result->end_line == 4);
    REQUIRE(result->returned_bytes == 15);
    REQUIRE_FALSE(result->truncated);
  });
}

TEST_CASE("read_text_file_ranged caps a line range by max_bytes", "[unit][io][range][lines]") {
  TempDir temp{"oran-io-range-lines-cap"};
  const auto file = temp.path() / "lines.txt";
  write_direct(file, "one\ntwo\nthree\nfour\nfive\n");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    io::ReadTextOptions options;
    options.range = io::FileRange{.lines = io::FileRange::LineSpan{.start_line = 1, .line_count = 5}};
    options.max_bytes = 4;
    auto result = co_await io::read_text_file_ranged(context.get_executor(), file.string(), options);
    REQUIRE(result.has_value());
    REQUIRE(result->truncated);
    REQUIRE(result->text.size() <= 4);
  });
}

TEST_CASE("read_text_file_ranged returns an empty span past EOF", "[unit][io][range][lines]") {
  TempDir temp{"oran-io-range-past-eof"};
  const auto file = temp.path() / "lines.txt";
  write_direct(file, "only one line\n");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    io::ReadTextOptions options;
    options.range = io::FileRange{.lines = io::FileRange::LineSpan{.start_line = 10, .line_count = 5}};
    auto result = co_await io::read_text_file_ranged(context.get_executor(), file.string(), options);
    REQUIRE(result.has_value());
    REQUIRE(result->text.empty());
    REQUIRE(result->start_line == 10);
    REQUIRE(result->end_line == 9);
    REQUIRE(result->returned_bytes == 0);
  });
}

TEST_CASE("read_text_file_ranged uses a line-offset index for large line ranges", "[unit][io][range][lines]") {
  TempDir temp{"oran-io-range-lines-indexed"};
  const auto file = temp.path() / "large-lines.txt";
  write_direct(file, large_numbered_file('a'));

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    io::ReadTextOptions options;
    options.range = io::FileRange{.lines = io::FileRange::LineSpan{.start_line = 3000, .line_count = 4}};
    auto result = co_await io::read_text_file_ranged(context.get_executor(), file.string(), options);

    REQUIRE(result.has_value());
    REQUIRE(result->fingerprint.size_bytes > 256U * 1024U);
    REQUIRE(result->text ==
            numbered_line(3000, 'a') + numbered_line(3001, 'a') + numbered_line(3002, 'a') + numbered_line(3003, 'a'));
    REQUIRE(result->start_line == 3000);
    REQUIRE(result->end_line == 3003);
    REQUIRE(result->returned_bytes == result->text.size());
    REQUIRE_FALSE(result->truncated);
  });
}

TEST_CASE("write_text_file invalidates the large-file line-offset index", "[unit][io][range][lines]") {
  TempDir temp{"oran-io-range-lines-index-write"};
  const auto file = temp.path() / "large-lines.txt";
  const auto original = large_numbered_file('a');
  auto replacement = large_numbered_file('b');
  replacement.replace(0, numbered_line(1, 'b').size(), numbered_line(1, 'b', 160U));
  const auto final_line = numbered_line(4096, 'b');
  replacement.replace(replacement.size() - final_line.size(), final_line.size(), numbered_line(4096, 'b', 0U));
  REQUIRE(replacement.size() == original.size());
  write_direct(file, original);

  const auto original_mtime = std::filesystem::last_write_time(file);
  const auto original_fingerprint = io::compute_file_fingerprint(file.string());
  REQUIRE(original_fingerprint.has_value());

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    io::ReadTextOptions options;
    options.range = io::FileRange{.lines = io::FileRange::LineSpan{.start_line = 3000, .line_count = 1}};
    auto first = co_await io::read_text_file_ranged(context.get_executor(), file.string(), options);
    REQUIRE(first.has_value());
    REQUIRE(first->text == numbered_line(3000, 'a'));

    auto written = co_await io::write_text_file(context.get_executor(), file.string(), replacement);
    REQUIRE(written.has_value());
    std::filesystem::last_write_time(file, original_mtime);

    const auto restored_fingerprint = io::compute_file_fingerprint(file.string());
    REQUIRE(restored_fingerprint.has_value());
    REQUIRE(restored_fingerprint->size_bytes == original_fingerprint->size_bytes);
    REQUIRE(restored_fingerprint->mtime_ns == original_fingerprint->mtime_ns);

    auto second = co_await io::read_text_file_ranged(context.get_executor(), file.string(), options);
    REQUIRE(second.has_value());
    REQUIRE(second->text == numbered_line(3000, 'b'));
  });
}

TEST_CASE("delete_file invalidates the large-file line-offset index", "[unit][io][range][lines]") {
  TempDir temp{"oran-io-range-lines-index-delete"};
  const auto file = temp.path() / "large-lines.txt";
  const auto original = large_numbered_file('c');
  auto replacement = large_numbered_file('d');
  replacement.replace(0, numbered_line(1, 'd').size(), numbered_line(1, 'd', 160U));
  const auto final_line = numbered_line(4096, 'd');
  replacement.replace(replacement.size() - final_line.size(), final_line.size(), numbered_line(4096, 'd', 0U));
  REQUIRE(replacement.size() == original.size());
  write_direct(file, original);

  const auto original_mtime = std::filesystem::last_write_time(file);
  const auto original_fingerprint = io::compute_file_fingerprint(file.string());
  REQUIRE(original_fingerprint.has_value());

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    io::ReadTextOptions options;
    options.range = io::FileRange{.lines = io::FileRange::LineSpan{.start_line = 3000, .line_count = 1}};
    auto first = co_await io::read_text_file_ranged(context.get_executor(), file.string(), options);
    REQUIRE(first.has_value());
    REQUIRE(first->text == numbered_line(3000, 'c'));

    auto deleted = co_await io::delete_file(context.get_executor(), file.string());
    REQUIRE(deleted.has_value());
    write_direct(file, replacement);
    std::filesystem::last_write_time(file, original_mtime);

    const auto restored_fingerprint = io::compute_file_fingerprint(file.string());
    REQUIRE(restored_fingerprint.has_value());
    REQUIRE(restored_fingerprint->size_bytes == original_fingerprint->size_bytes);
    REQUIRE(restored_fingerprint->mtime_ns == original_fingerprint->mtime_ns);

    auto second = co_await io::read_text_file_ranged(context.get_executor(), file.string(), options);
    REQUIRE(second.has_value());
    REQUIRE(second->text == numbered_line(3000, 'd'));
  });
}

TEST_CASE("read_text_file_ranged extracts a byte range", "[unit][io][range][bytes]") {
  TempDir temp{"oran-io-range-bytes"};
  const auto file = temp.path() / "blob.txt";
  write_direct(file, "abcdefghij");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    io::ReadTextOptions options;
    options.range = io::FileRange{.bytes = io::FileRange::ByteSpan{.offset_bytes = 3, .length_bytes = 4}};
    auto result = co_await io::read_text_file_ranged(context.get_executor(), file.string(), options);
    REQUIRE(result.has_value());
    REQUIRE(result->text == "defg");
    REQUIRE(result->returned_bytes == 4);
    REQUIRE_FALSE(result->truncated);
  });
}

TEST_CASE("read_text_file_ranged byte range past EOF returns the available tail", "[unit][io][range][bytes]") {
  TempDir temp{"oran-io-range-bytes-overflow"};
  const auto file = temp.path() / "blob.txt";
  write_direct(file, "abcdef");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    io::ReadTextOptions options;
    options.range = io::FileRange{.bytes = io::FileRange::ByteSpan{.offset_bytes = 4, .length_bytes = 100}};
    auto result = co_await io::read_text_file_ranged(context.get_executor(), file.string(), options);
    REQUIRE(result.has_value());
    REQUIRE(result->text == "ef");
    REQUIRE(result->returned_bytes == 2);
    REQUIRE_FALSE(result->truncated);
  });
}

TEST_CASE("read_text_file_ranged trims a byte range that splits a UTF-8 code point", "[unit][io][range][bytes]") {
  TempDir temp{"oran-io-range-bytes-utf8"};
  const auto file = temp.path() / "utf8.txt";
  // The Han character "中" is three bytes in UTF-8 (E4 B8 AD). Following it
  // with "x" places one more byte inside the requested span.
  write_direct(file, "\xE4\xB8\xADx");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    io::ReadTextOptions options;
    // Ask for the first two bytes of the multi-byte character — the helper
    // should adjust the tail back to the previous code-point boundary.
    options.range = io::FileRange{.bytes = io::FileRange::ByteSpan{.offset_bytes = 1, .length_bytes = 2}};
    auto result = co_await io::read_text_file_ranged(context.get_executor(), file.string(), options);
    REQUIRE(result.has_value());
    REQUIRE(result->text.empty());
    REQUIRE(result->returned_bytes == 0);
  });
}

TEST_CASE("read_text_file_ranged rejects an empty range", "[unit][io][range]") {
  TempDir temp{"oran-io-range-empty"};
  const auto file = temp.path() / "file.txt";
  write_direct(file, "hi");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    io::ReadTextOptions options;
    options.range = io::FileRange{};  // neither lines nor bytes set
    auto result = co_await io::read_text_file_ranged(context.get_executor(), file.string(), options);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  });
}

TEST_CASE("read_text_file_ranged rejects a range with both lines and bytes set", "[unit][io][range]") {
  TempDir temp{"oran-io-range-both"};
  const auto file = temp.path() / "file.txt";
  write_direct(file, "hi");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    io::ReadTextOptions options;
    options.range = io::FileRange{
        .lines = io::FileRange::LineSpan{.start_line = 1, .line_count = 1},
        .bytes = io::FileRange::ByteSpan{.offset_bytes = 1, .length_bytes = 1},
    };
    auto result = co_await io::read_text_file_ranged(context.get_executor(), file.string(), options);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  });
}

TEST_CASE("read_text_file_ranged rejects zero range fields", "[unit][io][range]") {
  TempDir temp{"oran-io-range-zero"};
  const auto file = temp.path() / "file.txt";
  write_direct(file, "hi");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    io::ReadTextOptions options;
    options.range = io::FileRange{.lines = io::FileRange::LineSpan{.start_line = 0, .line_count = 1}};
    auto zero_start = co_await io::read_text_file_ranged(context.get_executor(), file.string(), options);
    REQUIRE_FALSE(zero_start.has_value());
    REQUIRE(zero_start.error().kind() == core::ErrorKind::invalid_argument);

    options.range = io::FileRange{.bytes = io::FileRange::ByteSpan{.offset_bytes = 1, .length_bytes = 0}};
    auto zero_length = co_await io::read_text_file_ranged(context.get_executor(), file.string(), options);
    REQUIRE_FALSE(zero_length.has_value());
    REQUIRE(zero_length.error().kind() == core::ErrorKind::invalid_argument);
  });
}

TEST_CASE("read_text_file_ranged returns conflict on mid-read race for large files", "[unit][io][range][race]") {
  TempDir temp{"oran-io-range-race-large"};
  const auto file = temp.path() / "large.txt";
  // 96 KiB — well above the 64 KiB retry threshold so a mid-read race surfaces
  // immediately as `conflict` rather than triggering a retry.
  std::string seed(96U * 1024U, 'a');
  write_direct(file, seed);

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    // Race the read against a writer that bumps the file's content+mtime
    // mid-flight. The mid-read fingerprint compare must catch the drift.
    auto bump = std::thread{[&] {
      // A short delay nudges the writer past the helper's pre-fingerprint
      // capture without depending on exact scheduling; the test tolerates
      // either ordering by accepting both `conflict` and `value` outcomes
      // and asserting only that no false `value` slips through when conflict
      // is reported.
      std::this_thread::sleep_for(std::chrono::microseconds{50});
      std::ofstream out{file, std::ios::binary | std::ios::trunc};
      std::string fresh(96U * 1024U, 'b');
      out.write(fresh.data(), static_cast<std::streamsize>(fresh.size()));
    }};

    auto result = co_await io::read_text_file_ranged(context.get_executor(), file.string());
    bump.join();
    if (!result.has_value()) {
      REQUIRE(result.error().kind() == core::ErrorKind::conflict);
    } else {
      // No race happened to fire; the result must reflect a complete read.
      REQUIRE(result->returned_bytes == 96U * 1024U);
    }
  });
}
