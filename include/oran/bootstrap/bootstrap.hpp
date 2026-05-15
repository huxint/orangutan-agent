// include/oran/bootstrap/bootstrap.hpp — early runtime assembly entry points.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <oran/config.hpp>
#include <oran/core/result.hpp>

namespace orangutan::bootstrap {

enum class ConfigSource : std::uint8_t {
  built_in_defaults,
  default_file,
  explicit_file,
};

[[nodiscard]] std::string_view to_string_view(ConfigSource source) noexcept;

struct BootstrapOptions {
  std::span<const std::string_view> args{};
  std::string workspace{"."};
};

struct LoadedConfig {
  config::Config value{};
  ConfigSource source{ConfigSource::built_in_defaults};
  std::string path{};
};

[[nodiscard]] core::Result<LoadedConfig> load_config(BootstrapOptions options = {});
[[nodiscard]] core::Result<int> run(BootstrapOptions options = {});

}  // namespace orangutan::bootstrap
