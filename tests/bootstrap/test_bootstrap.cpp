// tests/bootstrap/test_bootstrap.cpp — config-aware bootstrap coverage.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <oran/bootstrap.hpp>
#include <oran/core/error.hpp>

namespace bootstrap = orangutan::bootstrap;
namespace core = orangutan::core;

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

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  auto out = std::ofstream{path};
  out << contents;
}

bootstrap::BootstrapOptions options(std::vector<std::string_view>& args, const std::filesystem::path& workspace) {
  return bootstrap::BootstrapOptions{
      .args = std::span<const std::string_view>{args},
      .workspace = workspace.string(),
  };
}

constexpr auto kConfigText = R"json(
{
  "runtime": {
    "workers": 3
  },
  "profiles": {
    "default": {
      "provider": "anthropic",
      "model": "claude-3-5-sonnet-latest",
      "base_url": "https://api.anthropic.com",
      "api_key_env": "ANTHROPIC_API_KEY"
    }
  },
  "routes": {
    "default": {
      "primary": "default",
      "fallbacks": []
    }
  }
}
)json";

}  // namespace

TEST_CASE("load_config uses built-in defaults when default config is absent", "[unit][bootstrap]") {
  TempDir temp{"oran-bootstrap-no-config"};
  auto args = std::vector<std::string_view>{};

  auto loaded = bootstrap::load_config(options(args, temp.path()));

  REQUIRE(loaded.has_value());
  REQUIRE(loaded->source == bootstrap::ConfigSource::built_in_defaults);
  REQUIRE(loaded->path.ends_with(".orangutan/config.json"));
  REQUIRE(loaded->value.runtime().workers == 4);
  REQUIRE(loaded->value.profiles().empty());
}

TEST_CASE("load_config loads workspace default config when present", "[unit][bootstrap]") {
  TempDir temp{"oran-bootstrap-default-config"};
  const auto config_path = temp.path() / ".orangutan" / "config.json";
  write_file(config_path, kConfigText);
  auto args = std::vector<std::string_view>{};

  auto loaded = bootstrap::load_config(options(args, temp.path()));

  REQUIRE(loaded.has_value());
  REQUIRE(loaded->source == bootstrap::ConfigSource::default_file);
  REQUIRE(loaded->path == config_path.string());
  REQUIRE(loaded->value.runtime().workers == 3);
  REQUIRE(loaded->value.profiles().size() == 1);
  REQUIRE(loaded->value.routes().size() == 1);
}

TEST_CASE("load_config honors explicit config arguments", "[unit][bootstrap]") {
  TempDir temp{"oran-bootstrap-explicit-config"};
  const auto config_path = temp.path() / "config.json";
  write_file(config_path, kConfigText);

  SECTION("--config path") {
    auto config_arg = config_path.string();
    auto args = std::vector<std::string_view>{"--config", config_arg};
    auto loaded = bootstrap::load_config(options(args, temp.path()));

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->source == bootstrap::ConfigSource::explicit_file);
    REQUIRE(loaded->path == config_path.string());
    REQUIRE(loaded->value.runtime().workers == 3);
  }

  SECTION("--config=path") {
    auto config_arg = std::string{"--config="}.append(config_path.string());
    auto args = std::vector<std::string_view>{config_arg};
    auto loaded = bootstrap::load_config(options(args, temp.path()));

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->source == bootstrap::ConfigSource::explicit_file);
    REQUIRE(loaded->path == config_path.string());
  }

  SECTION("xmake run separator") {
    auto config_arg = config_path.string();
    auto args = std::vector<std::string_view>{"--", "--config", config_arg};
    auto loaded = bootstrap::load_config(options(args, temp.path()));

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->source == bootstrap::ConfigSource::explicit_file);
    REQUIRE(loaded->path == config_path.string());
  }
}

TEST_CASE("load_config ignores CLI arguments while resolving config", "[unit][bootstrap]") {
  TempDir temp{"oran-bootstrap-cli-args"};
  auto args = std::vector<std::string_view>{"--prompt", "hello"};

  auto loaded = bootstrap::load_config(options(args, temp.path()));

  REQUIRE(loaded.has_value());
  REQUIRE(loaded->source == bootstrap::ConfigSource::built_in_defaults);
  REQUIRE(loaded->value.runtime().workers == 4);
}

TEST_CASE("load_config rejects invalid bootstrap arguments", "[unit][bootstrap]") {
  TempDir temp{"oran-bootstrap-invalid-args"};

  SECTION("missing explicit path") {
    auto args = std::vector<std::string_view>{"--config"};
    auto loaded = bootstrap::load_config(options(args, temp.path()));

    REQUIRE_FALSE(loaded.has_value());
    REQUIRE(loaded.error().kind() == core::ErrorKind::invalid_argument);
  }

  SECTION("missing explicit file") {
    auto missing_path = (temp.path() / "missing.json").string();
    auto args = std::vector<std::string_view>{"--config", missing_path};
    auto loaded = bootstrap::load_config(options(args, temp.path()));

    REQUIRE_FALSE(loaded.has_value());
    REQUIRE(loaded.error().kind() == core::ErrorKind::not_found);
  }
}

TEST_CASE("run handles help without loading config", "[unit][bootstrap]") {
  TempDir temp{"oran-bootstrap-help"};
  auto args = std::vector<std::string_view>{"--help"};

  auto result = bootstrap::run(options(args, temp.path()));

  REQUIRE(result.has_value());
  REQUIRE(*result == 0);
}

TEST_CASE("run hands CLI arguments to oran-cli after config load", "[unit][bootstrap]") {
  TempDir temp{"oran-bootstrap-cli-handoff"};
  auto args = std::vector<std::string_view>{"--prompt", "hello"};

  auto result = bootstrap::run(options(args, temp.path()));

  REQUIRE(result.has_value());
  REQUIRE(*result == 0);
}

TEST_CASE("run returns CLI argument errors after config load", "[unit][bootstrap]") {
  TempDir temp{"oran-bootstrap-cli-error"};
  auto args = std::vector<std::string_view>{"--unknown"};

  auto result = bootstrap::run(options(args, temp.path()));

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
}
