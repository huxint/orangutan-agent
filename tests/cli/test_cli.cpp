// tests/cli/test_cli.cpp — early CLI mode coverage.

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <oran/cli.hpp>
#include <oran/core/error.hpp>

namespace cli = orangutan::cli;
namespace core = orangutan::core;

namespace {

cli::CliOptions options(std::vector<std::string_view>& args) {
  return cli::CliOptions{.args = std::span<const std::string_view>{args}, .quiet = true};
}

cli::CliOptions options(std::vector<std::string_view>& args, std::vector<std::string_view>& repl_lines) {
  return cli::CliOptions{
      .args = std::span<const std::string_view>{args},
      .repl_lines = std::span<const std::string_view>{repl_lines},
      .quiet = true,
  };
}

}  // namespace

TEST_CASE("run selects REPL mode when no prompt is supplied", "[unit][cli]") {
  auto args = std::vector<std::string_view>{};

  auto result = cli::run(options(args));

  REQUIRE(result.has_value());
  REQUIRE(result->mode == cli::CliMode::repl);
  REQUIRE(result->prompts_processed == 0);
  REQUIRE(result->exit_code == 0);
}

TEST_CASE("run counts scripted REPL prompts", "[unit][cli]") {
  auto args = std::vector<std::string_view>{};
  auto repl_lines = std::vector<std::string_view>{"hello", "", "continue"};

  auto result = cli::run(options(args, repl_lines));

  REQUIRE(result.has_value());
  REQUIRE(result->mode == cli::CliMode::repl);
  REQUIRE(result->prompts_processed == 2);
}

TEST_CASE("run selects single-shot mode from prompt arguments", "[unit][cli]") {
  SECTION("--prompt value") {
    auto args = std::vector<std::string_view>{"--prompt", "What is 17 * 23?"};
    auto result = cli::run(options(args));

    REQUIRE(result.has_value());
    REQUIRE(result->mode == cli::CliMode::single_shot);
    REQUIRE(result->prompts_processed == 1);
  }

  SECTION("--prompt=value") {
    auto prompt_arg = std::string{"--prompt=Read README.md"};
    auto args = std::vector<std::string_view>{prompt_arg};
    auto result = cli::run(options(args));

    REQUIRE(result.has_value());
    REQUIRE(result->mode == cli::CliMode::single_shot);
    REQUIRE(result->prompts_processed == 1);
  }

  SECTION("xmake run separator") {
    auto args = std::vector<std::string_view>{"--", "--prompt", "hello"};
    auto result = cli::run(options(args));

    REQUIRE(result.has_value());
    REQUIRE(result->mode == cli::CliMode::single_shot);
    REQUIRE(result->prompts_processed == 1);
  }
}

TEST_CASE("run handles CLI help without dispatching a prompt", "[unit][cli]") {
  SECTION("help only") {
    auto args = std::vector<std::string_view>{"--help"};
    auto result = cli::run(options(args));

    REQUIRE(result.has_value());
    REQUIRE(result->mode == cli::CliMode::help);
    REQUIRE(result->prompts_processed == 0);
  }

  SECTION("help takes precedence over later args") {
    auto args = std::vector<std::string_view>{"--help", "--prompt", "ignored"};
    auto result = cli::run(options(args));

    REQUIRE(result.has_value());
    REQUIRE(result->mode == cli::CliMode::help);
    REQUIRE(result->prompts_processed == 0);
  }
}

TEST_CASE("run rejects invalid CLI arguments", "[unit][cli]") {
  SECTION("missing prompt value") {
    auto args = std::vector<std::string_view>{"--prompt"};
    auto result = cli::run(options(args));

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }

  SECTION("empty prompt value") {
    auto args = std::vector<std::string_view>{"--prompt", ""};
    auto result = cli::run(options(args));

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }

  SECTION("duplicate prompt") {
    auto args = std::vector<std::string_view>{"--prompt", "one", "--prompt", "two"};
    auto result = cli::run(options(args));

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }

  SECTION("unknown argument") {
    auto args = std::vector<std::string_view>{"--unknown"};
    auto result = cli::run(options(args));

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }
}
