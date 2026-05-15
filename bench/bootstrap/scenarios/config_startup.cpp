// bench/bootstrap/scenarios/config_startup.cpp
//
// A-vs-B comparison: missing default config startup vs. explicit config file load.

#include <nanobench.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <oran/bootstrap.hpp>

namespace orangutan::bench {
namespace bootstrap = orangutan::bootstrap;

namespace {

class BenchFixture {
public:
  BenchFixture()
      : root_(std::filesystem::temp_directory_path() /
              ("oran-bootstrap-bench-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))),
        explicit_config_(root_ / "config.json") {
    std::filesystem::create_directories(root_);
    auto out = std::ofstream{explicit_config_};
    out << R"json(
{
  "runtime": {
    "workers": 4
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
  }

  ~BenchFixture() {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  [[nodiscard]] bootstrap::BootstrapOptions default_options() const {
    return bootstrap::BootstrapOptions{.workspace = root_.string()};
  }

  [[nodiscard]] bootstrap::BootstrapOptions explicit_options() {
    explicit_arg_text_ = explicit_config_.string();
    explicit_args_ = std::vector<std::string_view>{"--config", explicit_arg_text_};
    return bootstrap::BootstrapOptions{
        .args = std::span<const std::string_view>{explicit_args_},
        .workspace = root_.string(),
    };
  }

private:
  std::filesystem::path root_;
  std::filesystem::path explicit_config_;
  std::string explicit_arg_text_{};
  std::vector<std::string_view> explicit_args_{};
};

[[gnu::noinline]] std::size_t load_missing_default(const bootstrap::BootstrapOptions& options) {
  auto loaded = bootstrap::load_config(options);
  if (!loaded) {
    std::abort();
  }
  return loaded->value.profiles().size() + static_cast<std::size_t>(loaded->source);
}

[[gnu::noinline]] std::size_t load_explicit_file(const bootstrap::BootstrapOptions& options) {
  auto loaded = bootstrap::load_config(options);
  if (!loaded) {
    std::abort();
  }
  return loaded->value.profiles().size() + loaded->value.routes().size() + static_cast<std::size_t>(loaded->source);
}

}  // namespace

void register_config_startup(ankerl::nanobench::Bench& bench) {
  auto fixture = BenchFixture{};
  const auto default_options = fixture.default_options();
  const auto explicit_options = fixture.explicit_options();

  bench.run("bootstrap.config_missing_default", [&] {
    const auto value = load_missing_default(default_options);
    ankerl::nanobench::doNotOptimizeAway(value);
  });
  bench.run("bootstrap.config_explicit_file", [&] {
    const auto value = load_explicit_file(explicit_options);
    ankerl::nanobench::doNotOptimizeAway(value);
  });
}

}  // namespace orangutan::bench
