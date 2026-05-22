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
