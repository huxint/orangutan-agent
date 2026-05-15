// include/oran/config/config.hpp — typed JSON configuration loader.
//
// The public surface stays third-party-free: nlohmann::json is confined to the
// implementation file so config users do not inherit parser compile cost.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <oran/core/result.hpp>

namespace orangutan::config {

struct ConfigWarning {
  std::string path;
  std::string message;
};

struct RuntimeConfig {
  std::int64_t workers{4};
  std::int64_t request_timeout_ms{600000};
  std::vector<std::string> redaction_patterns{};
};

struct ProfileConfig {
  std::string name;
  std::string provider;
  std::string model;
  std::string base_url;
  std::string api_key_env;
};

struct RouteConfig {
  std::string name;
  std::string primary_profile;
  std::vector<std::string> fallback_profiles{};
};

struct SessionConfig {
  bool auto_save{true};
  bool persistence{true};
};

struct WebConfig {
  bool enabled{false};
  std::string bind{"127.0.0.1"};
  std::int64_t port{8787};
};

struct LoadOptions {
  bool strict_unknown_fields{false};
};

class Config {
public:
  Config() = default;

  [[nodiscard]] static core::Result<Config> parse(std::string_view contents, LoadOptions options = {});
  [[nodiscard]] static core::Result<Config> load_file(std::string_view path, LoadOptions options = {});

  [[nodiscard]] bool strict_config() const noexcept {
    return strict_config_;
  }
  [[nodiscard]] const RuntimeConfig& runtime() const noexcept {
    return runtime_;
  }
  [[nodiscard]] std::span<const ProfileConfig> profiles() const noexcept {
    return std::span<const ProfileConfig>{profiles_};
  }
  [[nodiscard]] std::span<const RouteConfig> routes() const noexcept {
    return std::span<const RouteConfig>{routes_};
  }
  [[nodiscard]] const SessionConfig& session() const noexcept {
    return session_;
  }
  [[nodiscard]] const WebConfig& web() const noexcept {
    return web_;
  }
  [[nodiscard]] std::span<const ConfigWarning> warnings() const noexcept {
    return std::span<const ConfigWarning>{warnings_};
  }

private:
  bool strict_config_{false};
  RuntimeConfig runtime_{};
  std::vector<ProfileConfig> profiles_{};
  std::vector<RouteConfig> routes_{};
  SessionConfig session_{};
  WebConfig web_{};
  std::vector<ConfigWarning> warnings_{};
};

}  // namespace orangutan::config
