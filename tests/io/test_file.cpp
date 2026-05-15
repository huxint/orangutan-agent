// tests/io/test_file.cpp — file and directory helper coverage.

#include <chrono>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
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
