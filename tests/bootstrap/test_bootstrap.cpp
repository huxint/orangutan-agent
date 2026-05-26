// tests/bootstrap/test_bootstrap.cpp — config-aware bootstrap coverage.

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/bootstrap.hpp>
#include <oran/config/config.hpp>
#include <oran/core/error.hpp>
#include <oran/core/turn_id.hpp>
#include <oran/permission/rule_set.hpp>
#include <oran/storage.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace bootstrap = orangutan::bootstrap;
namespace config = orangutan::config;
namespace core = orangutan::core;
namespace permission = orangutan::permission;
namespace storage = orangutan::storage;
namespace test = orangutan::tests;
using asio::ip::tcp;

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

bool table_exists(const std::filesystem::path& db_path, std::string_view table) {
  auto connection = storage::Connection::open(storage::ConnectionOptions{
      .path = db_path.string(),
      .mode = storage::OpenMode::read_only,
      .enable_wal = false,
      .enforce_foreign_keys = false,
  });
  REQUIRE(connection.has_value());

  auto query = connection->prepare("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?");
  REQUIRE(query.has_value());
  REQUIRE(query->bind_text(1, table).has_value());
  auto row = query->step();
  REQUIRE(row.has_value());
  return *row == storage::StepResult::row;
}

bootstrap::BootstrapOptions options(std::vector<std::string_view>& args, const std::filesystem::path& workspace) {
  return bootstrap::BootstrapOptions{
      .args = std::span<const std::string_view>{args},
      .workspace = workspace.string(),
  };
}

std::optional<std::string_view> context_value(const core::Error& error, std::string_view key) {
  const auto it = std::ranges::find_if(error.context(), [&](const auto& entry) { return entry.first == key; });
  if (it == error.context().end()) {
    return std::nullopt;
  }
  return it->second;
}

class ScopedEnv {
public:
  ScopedEnv(std::string name, std::string value) : name_{std::move(name)} {
    if (const auto* old = std::getenv(name_.c_str()); old != nullptr) {
      old_value_ = old;
    }
    setenv(name_.c_str(), value.c_str(), 1);
  }

  ~ScopedEnv() {
    if (old_value_) {
      setenv(name_.c_str(), old_value_->c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
  }

  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
  std::string name_;
  std::optional<std::string> old_value_;
};

class ScopedUnsetEnv {
public:
  explicit ScopedUnsetEnv(std::string name) : name_{std::move(name)} {
    if (const auto* old = std::getenv(name_.c_str()); old != nullptr) {
      old_value_ = old;
      unsetenv(name_.c_str());
    }
  }

  ~ScopedUnsetEnv() {
    if (old_value_) {
      setenv(name_.c_str(), old_value_->c_str(), 1);
    }
  }

  ScopedUnsetEnv(const ScopedUnsetEnv&) = delete;
  ScopedUnsetEnv& operator=(const ScopedUnsetEnv&) = delete;

private:
  std::string name_;
  std::optional<std::string> old_value_;
};

class OneShotHttpServer {
public:
  explicit OneShotHttpServer(std::string response) : response_{std::move(response)} {
    tcp::endpoint endpoint{asio::ip::make_address("127.0.0.1"), 0};
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen(1);
    port_ = acceptor_.local_endpoint().port();
    worker_ = std::jthread{[this] { serve(); }};
  }

  ~OneShotHttpServer() {
    std::error_code ignored;
    acceptor_.close(ignored);
  }

  [[nodiscard]] std::string base_url() const {
    return "http://127.0.0.1:" + std::to_string(port_);
  }

  [[nodiscard]] std::string request_text() const {
    return request_;
  }

  [[nodiscard]] bool served() const noexcept {
    return served_.load();
  }

private:
  [[nodiscard]] static std::optional<std::size_t> content_length(std::string_view request) {
    constexpr auto kHeader = std::string_view{"Content-Length:"};
    const auto header = request.find(kHeader);
    if (header == std::string_view::npos) {
      return std::nullopt;
    }
    auto value = request.substr(header + kHeader.size());
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
      value.remove_prefix(1);
    }
    const auto end = value.find("\r\n");
    if (end != std::string_view::npos) {
      value = value.substr(0, end);
    }

    auto length = std::size_t{0};
    const auto* first = value.data();
    const auto* last = value.data() + value.size();
    const auto parsed = std::from_chars(first, last, length);
    if (parsed.ec != std::errc{} || parsed.ptr == first) {
      return std::nullopt;
    }
    return length;
  }

  [[nodiscard]] static bool request_complete(std::string_view request) {
    const auto header_end = request.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
      return false;
    }
    const auto length = content_length(request);
    if (!length) {
      return true;
    }
    return request.size() >= header_end + 4 + *length;
  }

  void serve() {
    std::error_code ec;
    auto socket = acceptor_.accept(ec);
    if (ec) {
      return;
    }

    std::array<char, 4096> buffer{};
    while (!request_complete(request_)) {
      const auto read = socket.read_some(asio::buffer(buffer), ec);
      if (ec && ec != asio::error::eof) {
        return;
      }
      if (read > 0) {
        request_.append(buffer.data(), read);
      }
      if (ec == asio::error::eof) {
        break;
      }
    }

    asio::write(socket, asio::buffer(response_), ec);
    served_ = !ec;
  }

  asio::io_context io_;
  tcp::acceptor acceptor_{io_};
  std::uint16_t port_{0};
  std::string response_;
  std::string request_;
  std::atomic_bool served_{false};
  std::jthread worker_;
};

std::string anthropic_response(std::string_view text = "binary ok") {
  auto body = std::string{
      R"json({"type":"message","role":"assistant","model":"claude-test","content":[{"type":"text","text":")json"};
  body.append(text);
  body.append(R"json("}],"stop_reason":"end_turn","usage":{"input_tokens":4,"output_tokens":2}})json");
  return "HTTP/1.1 200 OK\r\n"
         "Content-Type: application/json\r\n"
         "Content-Length: " +
         std::to_string(body.size()) +
         "\r\n"
         "Connection: close\r\n"
         "\r\n" +
         body;
}

std::string provider_config_text(std::string_view base_url) {
  auto text = std::string{R"json(
{
  "runtime": {
    "workers": 1,
    "request_timeout_ms": 2000
  },
  "profiles": {
    "default": {
      "provider": "anthropic",
      "protocol": "anthropic_messages",
      "model": "claude-test",
      "base_url": ")json"};
  text.append(base_url);
  text.append(R"json(",
      "api_key_env": "ORAN_BOOTSTRAP_RUN_PROVIDER_KEY"
    }
  },
  "routes": {
    "default": {
      "primary": "default",
      "fallbacks": []
    }
  }
}
)json");
  return text;
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

TEST_CASE("run hands configured provider prompts to AgentPromptRunner", "[unit][bootstrap][provider]") {
  ScopedEnv api_key{"ORAN_BOOTSTRAP_RUN_PROVIDER_KEY", "test-secret"};
  OneShotHttpServer server{anthropic_response()};
  TempDir temp{"oran-bootstrap-provider-runner"};
  const auto config_path = temp.path() / "config.json";
  write_file(config_path, provider_config_text(server.base_url()));
  auto config_arg = config_path.string();
  auto args = std::vector<std::string_view>{"--config", config_arg, "--prompt", "hello"};

  auto result = bootstrap::run(options(args, temp.path()));

  REQUIRE(result.has_value());
  REQUIRE(*result == 0);
  REQUIRE(server.served());
  REQUIRE(server.request_text().starts_with("POST /v1/messages HTTP/1.1"));
  REQUIRE(server.request_text().contains("x-api-key: test-secret"));
  REQUIRE(server.request_text().contains(R"json("model":"claude-test")json"));
  REQUIRE(server.request_text().contains(R"json("max_tokens":1024)json"));
  REQUIRE(server.request_text().contains(R"json("stream":false)json"));
}

TEST_CASE("run reports missing provider credentials before CLI async handoff", "[unit][bootstrap][provider]") {
  ScopedUnsetEnv missing{"ORAN_BOOTSTRAP_RUN_PROVIDER_KEY"};
  TempDir temp{"oran-bootstrap-provider-missing-credential"};
  const auto config_path = temp.path() / "config.json";
  write_file(config_path, provider_config_text("http://127.0.0.1:1"));
  auto config_arg = config_path.string();
  auto args = std::vector<std::string_view>{"--config", config_arg, "--prompt", "hello"};

  auto result = bootstrap::run(options(args, temp.path()));

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::auth);
  REQUIRE(context_value(result.error(), "profile") == std::optional<std::string_view>{"default"});
  REQUIRE(context_value(result.error(), "api_key_env") ==
          std::optional<std::string_view>{"ORAN_BOOTSTRAP_RUN_PROVIDER_KEY"});
}

TEST_CASE("run rejects invalid provider route config before CLI handoff", "[unit][bootstrap][provider]") {
  TempDir temp{"oran-bootstrap-provider-route-bad"};
  const auto config_path = temp.path() / "config.json";
  write_file(config_path, R"json(
{
  "profiles": {
    "bad": {
      "provider": "telepathy",
      "model": "unknown",
      "base_url": "http://127.0.0.1:1",
      "api_key_env": "BAD_API_KEY"
    }
  },
  "routes": {
    "default": {
      "primary": "bad"
    }
  }
}
)json");
  auto config_arg = config_path.string();
  auto args = std::vector<std::string_view>{"--config", config_arg, "--prompt", "hello"};

  auto result = bootstrap::run(options(args, temp.path()));

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::config);
}

TEST_CASE("run rejects invalid provider protocol config before CLI handoff", "[unit][bootstrap][provider]") {
  TempDir temp{"oran-bootstrap-provider-protocol-bad"};
  const auto config_path = temp.path() / "config.json";
  write_file(config_path, R"json(
{
  "profiles": {
    "bad": {
      "provider": "openai",
      "protocol": "responses-ish",
      "model": "unknown",
      "base_url": "http://127.0.0.1:1",
      "api_key_env": "BAD_API_KEY"
    }
  },
  "routes": {
    "default": {
      "primary": "bad"
    }
  }
}
)json");
  auto config_arg = config_path.string();
  auto args = std::vector<std::string_view>{"--config", config_arg, "--prompt", "hello"};

  auto result = bootstrap::run(options(args, temp.path()));

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::config);
  REQUIRE(context_value(result.error(), "profile") == std::optional<std::string_view>{"bad"});
  REQUIRE(context_value(result.error(), "role") == std::optional<std::string_view>{"primary"});
  REQUIRE(context_value(result.error(), "protocol") == std::optional<std::string_view>{"responses-ish"});
}

TEST_CASE("run rejects invalid provider adapter endpoint config before CLI handoff", "[unit][bootstrap][provider]") {
  TempDir temp{"oran-bootstrap-provider-adapter-bad"};
  const auto config_path = temp.path() / "config.json";
  write_file(config_path, R"json(
{
  "profiles": {
    "bad": {
      "provider": "anthropic",
      "protocol": "anthropic_messages",
      "model": "claude",
      "base_url": "ftp://api.example.invalid",
      "api_key_env": "ANTHROPIC_API_KEY"
    }
  },
  "routes": {
    "default": {
      "primary": "bad"
    }
  }
}
)json");
  auto config_arg = config_path.string();
  auto args = std::vector<std::string_view>{"--config", config_arg, "--prompt", "hello"};

  auto result = bootstrap::run(options(args, temp.path()));

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::config);
  REQUIRE(context_value(result.error(), "role") == std::optional<std::string_view>{"primary"});
  REQUIRE(context_value(result.error(), "profile") == std::optional<std::string_view>{"bad"});
  REQUIRE(context_value(result.error(), "field") == std::optional<std::string_view>{"base_url"});
}

TEST_CASE("run returns CLI argument errors after config load", "[unit][bootstrap]") {
  TempDir temp{"oran-bootstrap-cli-error"};
  auto args = std::vector<std::string_view>{"--unknown"};

  auto result = bootstrap::run(options(args, temp.path()));

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
}

TEST_CASE("run --audit-init applies the audit schema at the default workspace path", "[unit][bootstrap]") {
  TempDir temp{"oran-bootstrap-audit-init-default"};
  auto args = std::vector<std::string_view>{"--audit-init"};

  auto result = bootstrap::run(options(args, temp.path()));

  REQUIRE(result.has_value());
  REQUIRE(*result == 0);
  const auto expected_db = temp.path() / ".orangutan" / "audit.db";
  REQUIRE(std::filesystem::exists(expected_db));
  REQUIRE(table_exists(expected_db, "audit_events"));
  REQUIRE(table_exists(expected_db, "trace_turns"));

  // Idempotent on a second run.
  auto repeat = bootstrap::run(options(args, temp.path()));
  REQUIRE(repeat.has_value());
  REQUIRE(*repeat == 0);
}

TEST_CASE("run --audit-init honors an explicit audit-db path", "[unit][bootstrap]") {
  TempDir temp{"oran-bootstrap-audit-init-explicit"};
  const auto target = temp.path() / "nested" / "audit.db";
  auto target_str = target.string();

  SECTION("--audit-init <path>") {
    auto args = std::vector<std::string_view>{"--audit-init", target_str};
    auto result = bootstrap::run(options(args, temp.path()));
    REQUIRE(result.has_value());
    REQUIRE(*result == 0);
    REQUIRE(std::filesystem::exists(target));
  }

  SECTION("--audit-init=<path>") {
    auto arg = std::string{"--audit-init="}.append(target_str);
    auto args = std::vector<std::string_view>{arg};
    auto result = bootstrap::run(options(args, temp.path()));
    REQUIRE(result.has_value());
    REQUIRE(*result == 0);
    REQUIRE(std::filesystem::exists(target));
  }
}

TEST_CASE("run --audit-init rejects empty explicit paths", "[unit][bootstrap]") {
  TempDir temp{"oran-bootstrap-audit-init-empty"};

  SECTION("--audit-init=") {
    auto args = std::vector<std::string_view>{"--audit-init="};
    auto result = bootstrap::run(options(args, temp.path()));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }
}

TEST_CASE("run --audit-init refuses to swallow short flags as its path", "[unit][bootstrap]") {
  TempDir temp{"oran-bootstrap-audit-init-short-flag"};
  auto args = std::vector<std::string_view>{"--audit-init", "-h"};
  auto result = bootstrap::run(options(args, temp.path()));
  // `-h` is a help-shaped short flag and must not be consumed as the audit-init
  // path. With the strictness fix the optional-path sniff treats it as a flag
  // and falls through to ordinary help handling; the call now succeeds (help
  // text is printed) and the workspace audit directory is left untouched.
  REQUIRE(result.has_value());
  REQUIRE_FALSE(std::filesystem::exists(temp.path() / "-h"));
}

TEST_CASE("run rejects duplicate bootstrap flags", "[unit][bootstrap]") {
  TempDir temp{"oran-bootstrap-duplicate-flags"};

  SECTION("duplicate --config") {
    auto args = std::vector<std::string_view>{"--config", "a.json", "--config", "b.json"};
    auto result = bootstrap::run(options(args, temp.path()));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }
  SECTION("duplicate --audit-init") {
    auto args = std::vector<std::string_view>{"--audit-init", "--audit-init"};
    auto result = bootstrap::run(options(args, temp.path()));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }
  SECTION("duplicate --trace") {
    auto args = std::vector<std::string_view>{"--trace", "abc", "--trace", "def"};
    auto result = bootstrap::run(options(args, temp.path()));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }
}

TEST_CASE("parse_explain_rules_selector defaults to default-mode, no agent", "[unit][bootstrap][explain_rules]") {
  auto args = std::vector<std::string_view>{};
  auto selector = bootstrap::parse_explain_rules_selector(std::span<const std::string_view>{args});

  REQUIRE(selector.has_value());
  REQUIRE(selector->mode == permission::Mode::default_);
  REQUIRE(selector->agent_name.empty());
}

TEST_CASE("parse_explain_rules_selector accepts every documented mode", "[unit][bootstrap][explain_rules]") {
  const auto cases = std::vector<std::pair<std::string_view, permission::Mode>>{
      {"strict", permission::Mode::strict},
      {"default", permission::Mode::default_},
      {"permissive", permission::Mode::permissive},
      {"sandboxed", permission::Mode::sandboxed},
  };

  for (const auto& [name, expected] : cases) {
    auto args = std::vector<std::string_view>{"--mode", name};
    auto selector = bootstrap::parse_explain_rules_selector(std::span<const std::string_view>{args});
    REQUIRE(selector.has_value());
    REQUIRE(selector->mode == expected);
  }
}

TEST_CASE("parse_explain_rules_selector accepts --mode=<name>", "[unit][bootstrap][explain_rules]") {
  auto args = std::vector<std::string_view>{"--mode=strict"};
  auto selector = bootstrap::parse_explain_rules_selector(std::span<const std::string_view>{args});

  REQUIRE(selector.has_value());
  REQUIRE(selector->mode == permission::Mode::strict);
}

TEST_CASE("parse_explain_rules_selector accepts --agent <name> and --agent=<name>",
          "[unit][bootstrap][explain_rules]") {
  SECTION("space-separated form") {
    auto args = std::vector<std::string_view>{"--agent", "researcher"};
    auto selector = bootstrap::parse_explain_rules_selector(std::span<const std::string_view>{args});
    REQUIRE(selector.has_value());
    REQUIRE(selector->agent_name == "researcher");
  }
  SECTION("eq-form") {
    auto args = std::vector<std::string_view>{"--agent=researcher"};
    auto selector = bootstrap::parse_explain_rules_selector(std::span<const std::string_view>{args});
    REQUIRE(selector.has_value());
    REQUIRE(selector->agent_name == "researcher");
  }
}

TEST_CASE("parse_explain_rules_selector rejects unknown mode spellings", "[unit][bootstrap][explain_rules]") {
  auto args = std::vector<std::string_view>{"--mode", "lax"};
  auto selector = bootstrap::parse_explain_rules_selector(std::span<const std::string_view>{args});

  REQUIRE_FALSE(selector.has_value());
  REQUIRE(selector.error().kind() == core::ErrorKind::invalid_argument);
}

TEST_CASE("parse_explain_rules_selector rejects empty mode / agent values", "[unit][bootstrap][explain_rules]") {
  SECTION("missing --mode value") {
    auto args = std::vector<std::string_view>{"--mode"};
    auto selector = bootstrap::parse_explain_rules_selector(std::span<const std::string_view>{args});
    REQUIRE_FALSE(selector.has_value());
    REQUIRE(selector.error().kind() == core::ErrorKind::invalid_argument);
  }
  SECTION("empty --mode= value") {
    auto args = std::vector<std::string_view>{"--mode="};
    auto selector = bootstrap::parse_explain_rules_selector(std::span<const std::string_view>{args});
    REQUIRE_FALSE(selector.has_value());
    REQUIRE(selector.error().kind() == core::ErrorKind::invalid_argument);
  }
  SECTION("missing --agent value") {
    auto args = std::vector<std::string_view>{"--agent"};
    auto selector = bootstrap::parse_explain_rules_selector(std::span<const std::string_view>{args});
    REQUIRE_FALSE(selector.has_value());
    REQUIRE(selector.error().kind() == core::ErrorKind::invalid_argument);
  }
}

namespace {

constexpr auto kExplainConfigText = R"json(
{
  "permissions": {
    "allow": [
      {"tool_pattern": "file.read"}
    ],
    "deny": [
      {"tool_pattern": "*", "capability": "runtime_loader"}
    ]
  },
  "agents": {
    "researcher": {
      "permissions": {
        "allow": [
          {"tool_pattern": "*", "capability": "egress_http"}
        ]
      }
    }
  }
}
)json";

[[nodiscard]] config::Config load_explain_config() {
  auto parsed = config::Config::parse(kExplainConfigText);
  REQUIRE(parsed.has_value());
  return std::move(*parsed);
}

[[nodiscard]] bool
has_rule_for_capability(const permission::RuleSet& rs, permission::Verdict verdict, core::Capability capability) {
  for (const auto& rule : rs.rules()) {
    if (rule.verdict == verdict && rule.capability == capability) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST_CASE("materialize_rules without agent uses defaults + global only", "[unit][bootstrap][explain_rules]") {
  const auto cfg = load_explain_config();

  auto rs = bootstrap::materialize_rules(cfg, bootstrap::ExplainRulesSelector{.mode = permission::Mode::default_});

  REQUIRE(rs.has_value());
  // The researcher-only egress_http allow must NOT appear when no agent is selected.
  REQUIRE_FALSE(has_rule_for_capability(*rs, permission::Verdict::allow, core::Capability::egress_http));
}

TEST_CASE("materialize_rules applies a named agent overlay", "[unit][bootstrap][explain_rules]") {
  const auto cfg = load_explain_config();

  auto rs = bootstrap::materialize_rules(
      cfg,
      bootstrap::ExplainRulesSelector{.mode = permission::Mode::default_, .agent_name = "researcher"});

  REQUIRE(rs.has_value());
  REQUIRE(has_rule_for_capability(*rs, permission::Verdict::allow, core::Capability::egress_http));
}

TEST_CASE("materialize_rules respects mode-driven defaults", "[unit][bootstrap][explain_rules]") {
  const auto cfg = config::Config{};

  auto strict = bootstrap::materialize_rules(cfg, bootstrap::ExplainRulesSelector{.mode = permission::Mode::strict});
  auto def = bootstrap::materialize_rules(cfg, bootstrap::ExplainRulesSelector{.mode = permission::Mode::default_});
  auto permissive =
      bootstrap::materialize_rules(cfg, bootstrap::ExplainRulesSelector{.mode = permission::Mode::permissive});

  REQUIRE(strict.has_value());
  REQUIRE(def.has_value());
  REQUIRE(permissive.has_value());
  REQUIRE(strict->size() == 0);               // strict has no baseline rules
  REQUIRE(def->size() > permissive->size());  // default baseline is denser than permissive
}

TEST_CASE("materialize_rules surfaces unknown agent as not_found", "[unit][bootstrap][explain_rules]") {
  const auto cfg = load_explain_config();

  auto rs = bootstrap::materialize_rules(
      cfg,
      bootstrap::ExplainRulesSelector{.mode = permission::Mode::default_, .agent_name = "ghost"});

  REQUIRE_FALSE(rs.has_value());
  REQUIRE(rs.error().kind() == core::ErrorKind::not_found);
}

TEST_CASE("run --explain-rules accepts --mode and exits zero", "[unit][bootstrap][explain_rules]") {
  TempDir temp{"oran-bootstrap-explain-mode"};
  auto args = std::vector<std::string_view>{"--explain-rules", "--mode", "permissive"};

  auto result = bootstrap::run(options(args, temp.path()));

  REQUIRE(result.has_value());
  REQUIRE(*result == 0);
}

TEST_CASE("run --explain-rules rejects unknown --mode", "[unit][bootstrap][explain_rules]") {
  TempDir temp{"oran-bootstrap-explain-bad-mode"};
  auto args = std::vector<std::string_view>{"--explain-rules", "--mode", "yolo"};

  auto result = bootstrap::run(options(args, temp.path()));

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
}

TEST_CASE("run --explain-rules rejects unknown --agent", "[unit][bootstrap][explain_rules]") {
  TempDir temp{"oran-bootstrap-explain-bad-agent"};
  const auto config_path = temp.path() / "config.json";
  write_file(config_path, kExplainConfigText);
  auto config_arg = config_path.string();
  auto args = std::vector<std::string_view>{"--config", config_arg, "--explain-rules", "--agent", "ghost"};

  auto result = bootstrap::run(options(args, temp.path()));

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::not_found);
}

namespace {

constexpr std::string_view kSampleTurnHex = "101112131415161718191a1b1c1d1e1f";

core::TurnId sample_turn_id() {
  core::TurnId id{};
  for (std::size_t i = 0; i < id.size(); ++i) {
    id[i] = static_cast<std::byte>(0x10 + i);
  }
  return id;
}

core::TurnId session_id_seed(unsigned char seed) {
  core::TurnId id{};
  for (std::size_t i = 0; i < id.size(); ++i) {
    id[i] = static_cast<std::byte>(seed + i);
  }
  return id;
}

void populate_trace_fixture(const std::filesystem::path& audit_db, const core::TurnId& turn_id) {
  test::run_async([&audit_db, &turn_id](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = storage::Pool::open(
        io.get_executor(),
        storage::PoolOptions{.path = audit_db.string(), .reader_count = 1, .statement_cache_capacity = 4});
    REQUIRE(pool.has_value());

    storage::AuditRepository audit_repo{*pool};
    storage::TraceRepository trace_repo{*pool};
    auto migrated = co_await audit_repo.migrate();
    REQUIRE(migrated.has_value());

    auto trace_request = storage::AppendTraceTurnRequest{
        .turn_id = turn_id,
        .session_id = session_id_seed(0x80),
        .agent_key = "coder",
        .origin = "cli",
        .route_profile = "fake-main",
        .route_model = "fake-model",
        .started_at_ns = 1'000,
        .finished_at_ns = 2'500,
        .stop_reason = "end_turn",
        .iteration_count = 2,
        .prompt_prefix_hash = 0xfeed'face'1234'5678ULL,
        .prompt_prefix_bytes = 4'096,
        .active_catalog_hash = 0x1111'2222'3333'4444ULL,
        .deferred_catalog_hash = 0x5555'6666'7777'8888ULL,
        .cache_creation_tokens = 32,
        .cache_read_tokens = 1024,
        .input_tokens = 1'500,
        .output_tokens = 200,
        .cost_estimate_usd = 0.012,
        .context_json = R"json({"source":"trace-inspector-test"})json",
    };
    auto trace_row = co_await trace_repo.append_turn(std::move(trace_request));
    REQUIRE(trace_row.has_value());

    auto first_audit = storage::AppendAuditEventRequest{
        .scope_key = "scope-A",
        .agent_key = "coder",
        .tool_name = "file.read",
        .identity = "operator-1",
        .verdict = "allow",
        .outcome = "allow",
        .reason = "rule #1 (allow: file.*)",
        .parent_turn_id = turn_id,
    };
    auto first_audit_row = co_await audit_repo.append_event(std::move(first_audit));
    REQUIRE(first_audit_row.has_value());

    auto hook_publish = storage::AppendAuditEventRequest{
        .event_kind = "hook_publish",
        .scope_key = "scope-A",
        .agent_key = "coder",
        .tool_name = "file.write",
        .identity = "operator-1",
        .verdict = "allow",
        .outcome = "allow",
        .reason = "policy",
        .parent_turn_id = turn_id,
        .metadata_json = R"json({"event":"tool_before","sink_id":"policy","decision_kind":"veto"})json",
    };
    auto hook_publish_row = co_await audit_repo.append_event(std::move(hook_publish));
    REQUIRE(hook_publish_row.has_value());

    auto second_audit = storage::AppendAuditEventRequest{
        .scope_key = "scope-A",
        .agent_key = "coder",
        .tool_name = "file.write",
        .identity = "operator-1",
        .verdict = "allow",
        .outcome = "allow",
        .reason = "rule #1 (allow: file.*)",
        .parent_turn_id = turn_id,
    };
    auto second_audit_row = co_await audit_repo.append_event(std::move(second_audit));
    REQUIRE(second_audit_row.has_value());
    co_return;
  });
}

}  // namespace

TEST_CASE("run --trace rejects missing or empty turn id", "[unit][bootstrap][trace]") {
  TempDir temp{"oran-bootstrap-trace-no-id"};

  SECTION("--trace without value") {
    auto args = std::vector<std::string_view>{"--trace"};
    auto result = bootstrap::run(options(args, temp.path()));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }

  SECTION("--trace= empty value") {
    auto args = std::vector<std::string_view>{"--trace="};
    auto result = bootstrap::run(options(args, temp.path()));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }
}

TEST_CASE("run --trace rejects malformed turn ids", "[unit][bootstrap][trace]") {
  TempDir temp{"oran-bootstrap-trace-bad-id"};

  SECTION("wrong length") {
    auto args = std::vector<std::string_view>{"--trace", "deadbeef"};
    auto result = bootstrap::run(options(args, temp.path()));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }

  SECTION("uppercase hex") {
    auto args = std::vector<std::string_view>{"--trace", "101112131415161718191A1B1C1D1E1F"};
    auto result = bootstrap::run(options(args, temp.path()));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }

  SECTION("non-hex character") {
    auto args = std::vector<std::string_view>{"--trace", "101112131415161718191a1b1c1d1ezz"};
    auto result = bootstrap::run(options(args, temp.path()));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }

  SECTION("all zero") {
    auto args = std::vector<std::string_view>{"--trace", "00000000000000000000000000000000"};
    auto result = bootstrap::run(options(args, temp.path()));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }
}

TEST_CASE("run --trace reports not_found when the audit database is absent", "[unit][bootstrap][trace]") {
  TempDir temp{"oran-bootstrap-trace-no-db"};
  auto args = std::vector<std::string_view>{"--trace", std::string_view{kSampleTurnHex}};

  auto result = bootstrap::run(options(args, temp.path()));

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::not_found);
}

TEST_CASE("run --trace reports not_found for unknown turn ids", "[unit][bootstrap][trace]") {
  TempDir temp{"oran-bootstrap-trace-missing-turn"};
  auto init_args = std::vector<std::string_view>{"--audit-init"};
  auto init_result = bootstrap::run(options(init_args, temp.path()));
  REQUIRE(init_result.has_value());

  auto args = std::vector<std::string_view>{"--trace", std::string_view{kSampleTurnHex}};
  auto result = bootstrap::run(options(args, temp.path()));

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::not_found);
}

TEST_CASE("run --trace returns 0 when the turn row and joined audit rows exist", "[unit][bootstrap][trace]") {
  TempDir temp{"oran-bootstrap-trace-happy"};
  const auto audit_db = temp.path() / ".orangutan" / "audit.db";
  std::filesystem::create_directories(audit_db.parent_path());

  populate_trace_fixture(audit_db, sample_turn_id());

  SECTION("space-separated form") {
    auto args = std::vector<std::string_view>{"--trace", std::string_view{kSampleTurnHex}};
    auto result = bootstrap::run(options(args, temp.path()));
    REQUIRE(result.has_value());
    REQUIRE(*result == 0);
  }

  SECTION("eq-form") {
    const auto eq_arg = std::string{"--trace="}.append(kSampleTurnHex);
    auto args = std::vector<std::string_view>{eq_arg};
    auto result = bootstrap::run(options(args, temp.path()));
    REQUIRE(result.has_value());
    REQUIRE(*result == 0);
  }
}
