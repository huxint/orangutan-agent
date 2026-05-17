// src/oran-bootstrap/bootstrap.cpp — config-aware bootstrap entry point.

#include <oran/bootstrap/bootstrap.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <print>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <oran/async.hpp>
#include <oran/bootstrap/runtime_assembly.hpp>
#include <oran/cli.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/permission.hpp>
#include <oran/storage.hpp>

namespace orangutan::bootstrap {
namespace {

using ::orangutan::core::Error;
using ::orangutan::core::Result;

constexpr std::string_view kVersion = "2.0.0-slice15";
constexpr std::string_view kAuditDatabaseRelative = ".orangutan/audit.db";

struct ParsedArgs {
  bool help{false};
  bool explain_rules{false};
  bool audit_init{false};
  bool has_audit_init_path{false};
  std::string audit_init_path{};
  bool has_explicit_config{false};
  std::string explicit_config_path{};
  std::vector<std::string_view> cli_args{};
};

[[nodiscard]] Error arg_error(std::string message) {
  return Error::invalid_argument(std::move(message));
}

[[nodiscard]] Result<ParsedArgs> parse_args(std::span<const std::string_view> args) {
  auto parsed = ParsedArgs{};

  for (auto index = std::size_t{0}; index < args.size(); ++index) {
    const auto arg = args[index];
    if (arg == "--") {
      continue;
    }

    if (arg == "--help" || arg == "-h") {
      parsed.help = true;
      continue;
    }

    if (arg == "--explain-rules") {
      parsed.explain_rules = true;
      continue;
    }

    constexpr auto kAuditInitPrefix = std::string_view{"--audit-init="};
    if (arg.starts_with(kAuditInitPrefix)) {
      parsed.audit_init = true;
      parsed.has_audit_init_path = true;
      parsed.audit_init_path = std::string{arg.substr(kAuditInitPrefix.size())};
      if (parsed.audit_init_path.empty()) {
        return std::unexpected(arg_error("--audit-init requires a non-empty path"));
      }
      continue;
    }

    if (arg == "--audit-init") {
      parsed.audit_init = true;
      // The path argument is optional: when omitted, audit-init uses the
      // workspace default `<workspace>/.orangutan/audit.db`. Sniff the
      // next token only if it does not look like another flag.
      if (index + 1 < args.size() && !args[index + 1].starts_with("--")) {
        parsed.has_audit_init_path = true;
        parsed.audit_init_path = std::string{args[++index]};
        if (parsed.audit_init_path.empty()) {
          return std::unexpected(arg_error("--audit-init requires a non-empty path"));
        }
      }
      continue;
    }

    constexpr auto kConfigPrefix = std::string_view{"--config="};
    if (arg.starts_with(kConfigPrefix)) {
      parsed.has_explicit_config = true;
      parsed.explicit_config_path = std::string{arg.substr(kConfigPrefix.size())};
      if (parsed.explicit_config_path.empty()) {
        return std::unexpected(arg_error("--config requires a non-empty path"));
      }
      continue;
    }

    if (arg == "--config") {
      if (index + 1 >= args.size()) {
        return std::unexpected(arg_error("--config requires a path"));
      }
      parsed.has_explicit_config = true;
      parsed.explicit_config_path = std::string{args[++index]};
      if (parsed.explicit_config_path.empty()) {
        return std::unexpected(arg_error("--config requires a non-empty path"));
      }
      continue;
    }

    parsed.cli_args.push_back(arg);
  }

  return parsed;
}

[[nodiscard]] std::string default_config_path(std::string_view workspace) {
  auto path = std::filesystem::path{std::string{workspace}};
  path /= ".orangutan";
  path /= "config.json";
  return path.string();
}

[[nodiscard]] std::string default_audit_path(std::string_view workspace) {
  auto path = std::filesystem::path{std::string{workspace}};
  path /= kAuditDatabaseRelative;
  return path.string();
}

[[nodiscard]] Result<bool> path_exists(std::string_view path) {
  auto ec = std::error_code{};
  const auto exists = std::filesystem::exists(std::filesystem::path{std::string{path}}, ec);
  if (ec) {
    return std::unexpected(
        Error::io("failed to inspect config path").with("path", std::string{path}).with("detail", ec.message()));
  }
  return exists;
}

void print_usage() {
  std::println("orangutan v{}", kVersion);
  std::println(
      "usage: orangutan [--config <path>] [--explain-rules] [--audit-init [<path>]] [--prompt <text>] [--help]");
  std::println();
  std::println("The current bootstrap slice loads config, then hands CLI modes to oran-cli.");
  std::println("--explain-rules prints the materialized permission rule set and exits.");
  std::println("--audit-init applies the audit.db schema (defaults to <workspace>/.orangutan/audit.db) and exits.");
}

[[nodiscard]] Result<int> print_materialized_rules(const config::Config& cfg) {
  // Use the design-doc "default" mode for diagnostics — operators can pick
  // their actual runtime mode once bootstrap owns it. The empty per-agent
  // overlay matches the "no agent selected yet" call shape.
  auto rs = permission::materialize(permission::Mode::default_, cfg.permissions(), config::PermissionsConfig{});
  if (!rs) {
    return std::unexpected(std::move(rs.error()));
  }

  std::println("materialized rules (mode={}): {} total", core::enum_name(permission::Mode::default_), rs->size());
  std::size_t index = 0;
  for (const auto& rule : rs->rules()) {
    std::print("  #{:<3} {:<5} tool={}", index, core::enum_name(rule.verdict), rule.tool_pattern);
    if (rule.capability.has_value()) {
      std::print(" capability={}", core::enum_name(*rule.capability));
    }
    if (rule.input_pattern.has_value()) {
      std::print(" input=~\"{}\"", rule.input_pattern->pattern());
    }
    std::println();
    ++index;
  }
  return 0;
}

[[nodiscard]] Result<int> run_audit_init(std::string audit_path) {
  if (audit_path.empty()) {
    return std::unexpected(arg_error("audit init path must not be empty"));
  }

  auto path = std::filesystem::path{audit_path};
  if (auto parent = path.parent_path(); !parent.empty()) {
    auto ec = std::error_code{};
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      return std::unexpected(
          Error::io("failed to create audit directory").with("path", parent.string()).with("detail", ec.message()));
    }
  }

  // Drive the migration on a one-shot io_context. The full
  // `async::Runtime` thread pool exists for the agent loop; a single
  // io_context is the right shape for a one-shot operator command.
  asio::io_context io;
  auto pool_result =
      storage::Pool::open(io.get_executor(),
                          storage::PoolOptions{.path = audit_path, .reader_count = 1, .statement_cache_capacity = 4});
  if (!pool_result) {
    return std::unexpected(std::move(pool_result).error());
  }
  auto pool = std::move(*pool_result);
  storage::AuditRepository repo{pool};

  auto report = storage::MigrationReport{};
  auto migrate_error = std::optional<core::Error>{};
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        auto migrated = co_await repo.migrate();
        if (!migrated) {
          migrate_error = std::move(migrated).error();
          co_return;
        }
        report = std::move(*migrated);
        co_return;
      },
      asio::detached);
  io.run();

  if (migrate_error) {
    return std::unexpected(std::move(*migrate_error));
  }

  std::println("audit schema ready: version {} at {} ({} migrations applied)",
               report.current_version,
               audit_path,
               report.applied_versions.size());
  return 0;
}

}  // namespace

std::string_view to_string_view(ConfigSource source) noexcept {
  switch (source) {
    case ConfigSource::built_in_defaults:
      return "built-in-defaults";
    case ConfigSource::default_file:
      return "default-file";
    case ConfigSource::explicit_file:
      return "explicit-file";
  }
  return "unknown";
}

core::Result<LoadedConfig> load_config(BootstrapOptions options) {
  auto parsed = parse_args(options.args);
  if (!parsed) {
    return std::unexpected(std::move(parsed.error()));
  }

  if (parsed->help) {
    return LoadedConfig{};
  }

  if (options.workspace.empty()) {
    return std::unexpected(Error::invalid_argument("workspace path is empty"));
  }

  if (parsed->has_explicit_config) {
    auto loaded = config::Config::load_file(parsed->explicit_config_path);
    if (!loaded) {
      return std::unexpected(std::move(loaded.error()));
    }
    return LoadedConfig{
        .value = std::move(*loaded),
        .source = ConfigSource::explicit_file,
        .path = std::move(parsed->explicit_config_path),
    };
  }

  auto path = default_config_path(options.workspace);
  auto exists = path_exists(path);
  if (!exists) {
    return std::unexpected(std::move(exists.error()));
  }

  if (!*exists) {
    return LoadedConfig{
        .value = config::Config{},
        .source = ConfigSource::built_in_defaults,
        .path = std::move(path),
    };
  }

  auto loaded = config::Config::load_file(path);
  if (!loaded) {
    return std::unexpected(std::move(loaded.error()));
  }
  return LoadedConfig{
      .value = std::move(*loaded),
      .source = ConfigSource::default_file,
      .path = std::move(path),
  };
}

core::Result<int> run(BootstrapOptions options) {
  auto parsed = parse_args(options.args);
  if (!parsed) {
    return std::unexpected(std::move(parsed.error()));
  }

  if (parsed->help) {
    print_usage();
    return 0;
  }

  if (parsed->audit_init) {
    auto audit_path =
        parsed->has_audit_init_path ? std::move(parsed->audit_init_path) : default_audit_path(options.workspace);
    return run_audit_init(std::move(audit_path));
  }

  auto loaded = load_config(options);
  if (!loaded) {
    return std::unexpected(std::move(loaded.error()));
  }

  if (parsed->explain_rules) {
    return print_materialized_rules(loaded->value);
  }

  std::println("orangutan v{}", kVersion);
  std::println("core, async, io, storage, migration, config, and bootstrap foundations are assembled;");
  std::println("config source: {} ({})", to_string_view(loaded->source), loaded->path);
  std::println("config summary: profiles={}, routes={}, workers={}, web={}",
               loaded->value.profiles().size(),
               loaded->value.routes().size(),
               loaded->value.runtime().workers,
               loaded->value.web().enabled ? "enabled" : "disabled");
  if (!loaded->value.warnings().empty()) {
    std::println("config warnings: {}", loaded->value.warnings().size());
  }

  // The runtime assembly composes the per-process permission infrastructure
  // (`ApprovalBroker`, audit `Pool`, `AuditRepository`, `StorageAuditSink`)
  // the upcoming agent loop will inherit. Slice 14 wired it in; slice 15
  // packages the audit migrations into the binary via `#embed`, so the
  // assembly can default to `audit_enabled=true` regardless of CWD.
  auto runtime = async::Runtime{async::RuntimeConfig{
      .io_workers = static_cast<std::size_t>(std::max<std::int64_t>(1, loaded->value.runtime().workers)),
      .cpu_workers = 1,
  }};
  auto assembly = RuntimeAssembly::build(options.workspace, runtime.executor(), RuntimeAssemblyOptions{});
  if (!assembly) {
    return std::unexpected(std::move(assembly).error());
  }
  std::println("runtime assembly ready: audit={} ({}), approval-broker=fresh",
               assembly->audit_enabled() ? "enabled" : "disabled",
               assembly->audit_enabled() ? assembly->audit_path() : std::string_view{"<null sink>"});

  auto cli_result = cli::run(cli::CliOptions{.args = std::span<const std::string_view>{parsed->cli_args}});
  if (!cli_result) {
    return std::unexpected(std::move(cli_result.error()));
  }
  return cli_result->exit_code;
}

}  // namespace orangutan::bootstrap
