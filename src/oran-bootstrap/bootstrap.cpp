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
#include <oran/bootstrap/signal_drain.hpp>
#include <oran/cli.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/permission.hpp>
#include <oran/storage.hpp>
#include <oran/tool/workspace.hpp>

namespace orangutan::bootstrap {
namespace {

using ::orangutan::core::Error;
using ::orangutan::core::Result;

constexpr std::string_view kVersion = "2.0.0-slice55";
constexpr std::string_view kAuditDatabaseRelative = ".orangutan/audit.db";

struct ParsedArgs {
  bool help{false};
  bool explain_rules{false};
  bool audit_init{false};
  bool has_audit_init_path{false};
  std::string audit_init_path{};
  bool has_explicit_config{false};
  std::string explicit_config_path{};
  ExplainRulesSelector explain_selector{};
  std::vector<std::string_view> cli_args{};
};

[[nodiscard]] Error arg_error(std::string message) {
  return Error::invalid_argument(std::move(message));
}

/// Pull the value following a long flag — supports both `--flag value` and
/// `--flag=value`. `index` advances past the consumed value token when the
/// space-separated form fired. Returns `invalid_argument` when the value is
/// missing or empty.
[[nodiscard]] Result<std::string>
consume_value(std::span<const std::string_view> args, std::size_t& index, std::string_view flag) {
  const auto arg = args[index];
  const auto eq_prefix = std::string{flag} + "=";
  if (arg.starts_with(eq_prefix)) {
    auto value = std::string{arg.substr(eq_prefix.size())};
    if (value.empty()) {
      return std::unexpected(arg_error(std::string{flag} + " requires a non-empty value"));
    }
    return value;
  }
  if (arg == flag) {
    if (index + 1 >= args.size()) {
      return std::unexpected(arg_error(std::string{flag} + " requires a value"));
    }
    auto value = std::string{args[++index]};
    if (value.empty()) {
      return std::unexpected(arg_error(std::string{flag} + " requires a non-empty value"));
    }
    return value;
  }
  return std::unexpected(arg_error("internal: consume_value invoked on non-matching flag"));
}

[[nodiscard]] bool matches_flag(std::string_view arg, std::string_view flag) noexcept {
  return arg == flag || arg.starts_with(std::string{flag} + "=");
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

    if (matches_flag(arg, "--mode")) {
      auto value = consume_value(args, index, "--mode");
      if (!value) {
        return std::unexpected(std::move(value.error()));
      }
      auto mode = core::parse_enum<permission::Mode>(*value);
      if (!mode) {
        return std::unexpected(arg_error("--mode does not match a known permission mode").with("value", *value));
      }
      parsed.explain_selector.mode = *mode;
      continue;
    }

    if (matches_flag(arg, "--agent")) {
      auto value = consume_value(args, index, "--agent");
      if (!value) {
        return std::unexpected(std::move(value.error()));
      }
      parsed.explain_selector.agent_name = std::move(*value);
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
  std::println("usage: orangutan [--config <path>] [--explain-rules [--mode <m>] [--agent <name>]]");
  std::println("                  [--audit-init [<path>]] [--prompt <text>] [--help]");
  std::println();
  std::println("The current bootstrap slice loads config, then hands CLI modes to oran-cli.");
  std::println("--explain-rules prints the materialized permission rule set and exits;");
  std::println("                --mode picks the baseline (strict|default|permissive|sandboxed),");
  std::println("                --agent picks an `agents.<name>.permissions` overlay.");
  std::println("--audit-init applies the audit.db schema (defaults to <workspace>/.orangutan/audit.db) and exits.");
}

[[nodiscard]] Result<int> print_materialized_rules(const config::Config& cfg, const ExplainRulesSelector& selector) {
  auto rs = materialize_rules(cfg, selector);
  if (!rs) {
    return std::unexpected(std::move(rs.error()));
  }

  std::print("materialized rules (mode={}", core::enum_name(selector.mode));
  if (!selector.agent_name.empty()) {
    std::print(", agent={}", selector.agent_name);
  }
  std::println("): {} total", rs->size());
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
  SignalScope signals{io};

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
        } else {
          report = std::move(*migrated);
        }
        signals.release();
        co_return;
      },
      asio::detached);
  io.run();

  if (const auto sig = signals.signum(); sig != 0) {
    return std::unexpected(
        core::Error::cancelled().with("signal", std::string{signal_name(sig)}).with("signum", std::to_string(sig)));
  }

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

core::Result<ExplainRulesSelector> parse_explain_rules_selector(std::span<const std::string_view> args) {
  auto parsed = parse_args(args);
  if (!parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  return parsed->explain_selector;
}

core::Result<permission::RuleSet> materialize_rules(const config::Config& cfg, const ExplainRulesSelector& selector) {
  if (selector.agent_name.empty()) {
    return permission::materialize(selector.mode, cfg.permissions(), config::PermissionsConfig{});
  }

  const auto agents = cfg.agents();
  const auto match =
      std::ranges::find_if(agents, [&](const config::AgentConfig& agent) { return agent.name == selector.agent_name; });
  if (match == agents.end()) {
    return std::unexpected(
        Error::not_found("--agent does not match a configured agent").with("agent", selector.agent_name));
  }
  return permission::materialize(selector.mode, cfg.permissions(), match->permissions);
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
    auto audit_result = run_audit_init(std::move(audit_path));
    if (!audit_result && audit_result.error().kind() == core::ErrorKind::cancelled) {
      if (auto signum = signum_from_error(audit_result.error()); signum) {
        std::println(stderr, "orangutan: interrupted by {} ({})", signal_name(*signum), *signum);
        return 128 + *signum;
      }
    }
    return audit_result;
  }

  auto loaded = load_config(options);
  if (!loaded) {
    return std::unexpected(std::move(loaded.error()));
  }

  if (parsed->explain_rules) {
    return print_materialized_rules(loaded->value, parsed->explain_selector);
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
  // plus the file-tool `tool::Workspace` the upcoming agent loop will
  // inherit. Slice 41 routes
  // `permissions.workspace.extra_{read,write}_roots` from `oran-config` into
  // `tool::WorkspaceOptions` here so the workspace canonicalises overrides
  // once at boot instead of per-tool.
  auto runtime = async::Runtime{async::RuntimeConfig{
      .io_workers = static_cast<std::size_t>(std::max<std::int64_t>(1, loaded->value.runtime().workers)),
      .cpu_workers = 1,
  }};
  auto assembly_options = RuntimeAssemblyOptions{};
  assembly_options.workspace_options = tool::WorkspaceOptions{
      .extra_read_roots = loaded->value.permissions().workspace.extra_read_roots,
      .extra_write_roots = loaded->value.permissions().workspace.extra_write_roots,
  };
  auto assembly = RuntimeAssembly::build(options.workspace, runtime.executor(), std::move(assembly_options));
  if (!assembly) {
    return std::unexpected(std::move(assembly).error());
  }
  std::println("runtime assembly ready: audit={} ({}), approval-broker=fresh, workspace={}",
               assembly->audit_enabled() ? "enabled" : "disabled",
               assembly->audit_enabled() ? assembly->audit_path() : std::string_view{"<null sink>"},
               assembly->workspace().root());

  auto cli_result = cli::run(cli::CliOptions{.args = std::span<const std::string_view>{parsed->cli_args}});
  if (!cli_result) {
    return std::unexpected(std::move(cli_result.error()));
  }
  return cli_result->exit_code;
}

}  // namespace orangutan::bootstrap
