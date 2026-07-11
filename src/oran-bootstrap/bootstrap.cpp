// src/oran-bootstrap/bootstrap.cpp — config-aware bootstrap entry point.

#include <oran/bootstrap/bootstrap.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <nlohmann/json.hpp>

#include <oran/async.hpp>
#include <oran/bootstrap/automation_cron.hpp>
#include <oran/bootstrap/memory_retention.hpp>
#include <oran/bootstrap/prompt_runner.hpp>
#include <oran/bootstrap/provider_backend.hpp>
#include <oran/bootstrap/runtime_assembly.hpp>
#include <oran/bootstrap/serve.hpp>
#include <oran/bootstrap/signal_drain.hpp>
#include <oran/cli.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/core/time.hpp>
#include <oran/core/turn_id.hpp>
#include <oran/desktop/chat_bridge.hpp>
#include <oran/desktop/shell.hpp>
#include <oran/hook.hpp>
#include <oran/http.hpp>
#include <oran/memory/longterm.hpp>
#include <oran/permission.hpp>
#include <oran/provider.hpp>
#include <oran/provider/fake.hpp>
#include <oran/storage.hpp>
#include <oran/tool/workspace.hpp>

namespace orangutan::bootstrap {
namespace {

using ::orangutan::core::Error;
using ::orangutan::core::Result;

constexpr std::string_view kVersion = "2.0.0-slice273";
constexpr std::string_view kAuditDatabaseRelative = ".orangutan/audit.db";
constexpr std::string_view kSkillsDirectoryRelative = ".orangutan/skills";
constexpr std::size_t kTraceExportDefaultLimit = 50;
constexpr std::string_view kLongtermTextEmbeddingModel = "oran-local-text-v1";
constexpr std::size_t kLongtermTextEmbeddingDimensions = 64;
constexpr std::int64_t kNanosecondsPerDay = 86'400'000'000'000;

struct ParsedArgs {
  bool help{false};
  bool desktop{false};
  bool serve{false};
  bool explain_rules{false};
  bool selector_mode_supplied{false};
  bool selector_agent_supplied{false};
  bool audit_init{false};
  bool has_audit_init_path{false};
  std::string audit_init_path{};
  bool trace_inspect{false};
  bool trace_export{false};
  std::string trace_turn_id_hex{};
  bool trace_export_limit_supplied{false};
  std::size_t trace_export_limit{kTraceExportDefaultLimit};
  bool trace_export_file_supplied{false};
  std::string trace_export_file_path{};
  bool trace_export_post_supplied{false};
  std::string trace_export_post_url{};
  bool has_explicit_config{false};
  std::string explicit_config_path{};
  ExplainRulesSelector explain_selector{};
  std::vector<std::string_view> cli_args{};
};

[[nodiscard]] Error arg_error(std::string message) {
  return Error::invalid_argument(std::move(message));
}

[[nodiscard]] std::int64_t retention_started_before_ns(std::int64_t retention_days) noexcept {
  using namespace std::chrono;
  const auto now_ns =
      duration_cast<nanoseconds>(core::time::now_utc().to_system_time_point().time_since_epoch()).count();
  if (retention_days <= 0 || retention_days > now_ns / kNanosecondsPerDay) {
    return 0;
  }
  return now_ns - (retention_days * kNanosecondsPerDay);
}

[[nodiscard]] Result<void> validate_longterm_recall_kinds(const std::vector<std::string>& names) {
  auto seen = std::vector<memory::longterm::RecordKind>{};
  seen.reserve(names.size());
  for (const auto& name : names) {
    auto parsed = core::parse_enum<memory::longterm::RecordKind>(name);
    if (!parsed) {
      return std::unexpected(Error::config("unknown long-term memory recall kind")
                                 .with("path", "$.memory.longterm.recall.kinds")
                                 .with("value", name));
    }
    if (std::ranges::contains(seen, *parsed)) {
      return std::unexpected(Error::config("duplicate long-term memory recall kind")
                                 .with("path", "$.memory.longterm.recall.kinds")
                                 .with("value", name));
    }
    seen.push_back(*parsed);
  }
  return {};
}

[[nodiscard]] LongtermRecallQueryStrategy
longterm_recall_query_strategy_from(config::LongtermMemoryRecallQueryStrategy strategy) noexcept {
  switch (strategy) {
    case config::LongtermMemoryRecallQueryStrategy::prompt_text:
      return LongtermRecallQueryStrategy::prompt_text;
    case config::LongtermMemoryRecallQueryStrategy::last_user_message:
      return LongtermRecallQueryStrategy::last_user_message;
  }
  return LongtermRecallQueryStrategy::prompt_text;
}

[[nodiscard]] Result<LongtermRecallOptions> longterm_recall_options_from(const config::Config& cfg) {
  const auto& recall = cfg.memory().longterm.recall;
  if (static_cast<std::uint64_t>(recall.limit) > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return std::unexpected(Error::config("memory.longterm.recall.limit exceeds platform size range")
                               .with("path", "$.memory.longterm.recall.limit")
                               .with("value", std::to_string(recall.limit)));
  }
  if (auto valid = validate_longterm_recall_kinds(recall.kinds); !valid) {
    return std::unexpected(std::move(valid.error()));
  }
  return LongtermRecallOptions{
      .enabled = recall.enabled,
      .limit = static_cast<std::size_t>(recall.limit),
      .query_strategy = longterm_recall_query_strategy_from(recall.query_strategy),
      .kinds = recall.kinds,
  };
}

[[nodiscard]] Result<void> validate_longterm_hybrid_search_policy(const config::Config& cfg) {
  if (!cfg.memory().longterm.hybrid_search.enabled) {
    return {};
  }
#if defined(ORAN_ENABLE_SQLITE_VEC)
  return {};
#else
  return std::unexpected(Error::config("memory.longterm.hybrid_search.enabled requires a vector memory backend")
                             .with("path", "$.memory.longterm.hybrid_search.enabled")
                             .with("reason", "build_option_disabled")
                             .with("option", "vector_memory"));
#endif
}

[[nodiscard]] Result<std::size_t> checked_memory_policy_size(std::int64_t value, std::string path) {
  if (static_cast<std::uint64_t>(value) > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return std::unexpected(Error::config("memory policy value exceeds platform size range")
                               .with("path", std::move(path))
                               .with("value", std::to_string(value)));
  }
  return static_cast<std::size_t>(value);
}

[[nodiscard]] Result<LongtermHybridSearchOptions> longterm_hybrid_search_options_from(const config::Config& cfg) {
  const auto& hybrid = cfg.memory().longterm.hybrid_search;
  auto lexical_limit =
      checked_memory_policy_size(hybrid.lexical_limit, "$.memory.longterm.hybrid_search.lexical_limit");
  if (!lexical_limit) {
    return std::unexpected(std::move(lexical_limit).error());
  }
  auto vector_limit = checked_memory_policy_size(hybrid.vector_limit, "$.memory.longterm.hybrid_search.vector_limit");
  if (!vector_limit) {
    return std::unexpected(std::move(vector_limit).error());
  }
  auto result_limit = checked_memory_policy_size(hybrid.result_limit, "$.memory.longterm.hybrid_search.result_limit");
  if (!result_limit) {
    return std::unexpected(std::move(result_limit).error());
  }
  return LongtermHybridSearchOptions{
      .enabled = hybrid.enabled,
      .lexical_limit = *lexical_limit,
      .vector_limit = *vector_limit,
      .result_limit = *result_limit,
      .lexical_weight = hybrid.lexical_weight,
      .vector_weight = hybrid.vector_weight,
      .embedding_model = std::string{kLongtermTextEmbeddingModel},
      .embedding_dimensions = kLongtermTextEmbeddingDimensions,
  };
}

[[nodiscard]] std::string longterm_startup_decay_summary(const RuntimeAssembly& assembly) {
  if (auto shadowed = assembly.longterm_memory_startup_decay_shadowed_count(); shadowed.has_value()) {
    return std::to_string(*shadowed);
  }
  return "disabled";
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

[[nodiscard]] Result<std::size_t> parse_positive_size(std::string_view text, std::string_view flag) {
  auto value = std::size_t{};
  const auto* const begin = text.data();
  const auto* const end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end) {
    return std::unexpected(arg_error(std::string{flag} + " requires a positive integer"));
  }
  if (value == 0) {
    return std::unexpected(arg_error(std::string{flag} + " must be greater than zero"));
  }
  return value;
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

    if (arg == "--desktop") {
      parsed.desktop = true;
      continue;
    }

    if (arg == "--serve") {
      parsed.serve = true;
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
      parsed.selector_mode_supplied = true;
      continue;
    }

    if (matches_flag(arg, "--agent")) {
      auto value = consume_value(args, index, "--agent");
      if (!value) {
        return std::unexpected(std::move(value.error()));
      }
      parsed.explain_selector.agent_name = std::move(*value);
      parsed.selector_agent_supplied = true;
      continue;
    }

    constexpr auto kAuditInitPrefix = std::string_view{"--audit-init="};
    if (arg.starts_with(kAuditInitPrefix)) {
      if (parsed.audit_init) {
        return std::unexpected(arg_error("--audit-init may be provided only once"));
      }
      parsed.audit_init = true;
      parsed.has_audit_init_path = true;
      parsed.audit_init_path = std::string{arg.substr(kAuditInitPrefix.size())};
      if (parsed.audit_init_path.empty()) {
        return std::unexpected(arg_error("--audit-init requires a non-empty path"));
      }
      continue;
    }

    if (arg == "--audit-init") {
      if (parsed.audit_init) {
        return std::unexpected(arg_error("--audit-init may be provided only once"));
      }
      parsed.audit_init = true;
      // The path argument is optional: when omitted, audit-init uses the
      // workspace default `<workspace>/.orangutan/audit.db`. Sniff the
      // next token only if it does not look like another flag (any token
      // starting with `-` — including single-dash short flags such as `-h`).
      if (index + 1 < args.size() && !args[index + 1].starts_with("-")) {
        parsed.has_audit_init_path = true;
        parsed.audit_init_path = std::string{args[++index]};
        if (parsed.audit_init_path.empty()) {
          return std::unexpected(arg_error("--audit-init requires a non-empty path"));
        }
      }
      continue;
    }

    constexpr auto kTracePrefix = std::string_view{"--trace="};
    if (arg.starts_with(kTracePrefix)) {
      if (parsed.trace_inspect || parsed.trace_export) {
        return std::unexpected(arg_error("--trace and --trace-export are mutually exclusive"));
      }
      parsed.trace_inspect = true;
      parsed.trace_turn_id_hex = std::string{arg.substr(kTracePrefix.size())};
      if (parsed.trace_turn_id_hex.empty()) {
        return std::unexpected(arg_error("--trace requires a non-empty turn id"));
      }
      continue;
    }

    if (arg == "--trace") {
      if (parsed.trace_inspect || parsed.trace_export) {
        return std::unexpected(arg_error("--trace and --trace-export are mutually exclusive"));
      }
      parsed.trace_inspect = true;
      if (index + 1 >= args.size()) {
        return std::unexpected(arg_error("--trace requires a turn id"));
      }
      parsed.trace_turn_id_hex = std::string{args[++index]};
      if (parsed.trace_turn_id_hex.empty()) {
        return std::unexpected(arg_error("--trace requires a non-empty turn id"));
      }
      continue;
    }

    constexpr auto kTraceExportPrefix = std::string_view{"--trace-export="};
    if (arg.starts_with(kTraceExportPrefix)) {
      if (parsed.trace_inspect || parsed.trace_export) {
        return std::unexpected(arg_error("--trace and --trace-export are mutually exclusive"));
      }
      parsed.trace_export = true;
      parsed.trace_turn_id_hex = std::string{arg.substr(kTraceExportPrefix.size())};
      if (parsed.trace_turn_id_hex.empty()) {
        return std::unexpected(arg_error("--trace-export requires a non-empty turn id"));
      }
      continue;
    }

    if (arg == "--trace-export") {
      if (parsed.trace_inspect || parsed.trace_export) {
        return std::unexpected(arg_error("--trace and --trace-export are mutually exclusive"));
      }
      parsed.trace_export = true;
      if (index + 1 < args.size() && !args[index + 1].starts_with("-")) {
        parsed.trace_turn_id_hex = std::string{args[++index]};
        if (parsed.trace_turn_id_hex.empty()) {
          return std::unexpected(arg_error("--trace-export requires a non-empty turn id"));
        }
      }
      continue;
    }

    if (matches_flag(arg, "--limit")) {
      if (parsed.trace_export_limit_supplied) {
        return std::unexpected(arg_error("--limit may be provided only once"));
      }
      auto value = consume_value(args, index, "--limit");
      if (!value) {
        return std::unexpected(std::move(value.error()));
      }
      auto limit = parse_positive_size(*value, "--limit");
      if (!limit) {
        return std::unexpected(std::move(limit.error()));
      }
      parsed.trace_export_limit_supplied = true;
      parsed.trace_export_limit = *limit;
      continue;
    }

    if (matches_flag(arg, "--trace-export-file")) {
      if (parsed.trace_export_file_supplied) {
        return std::unexpected(arg_error("--trace-export-file may be provided only once"));
      }
      auto value = consume_value(args, index, "--trace-export-file");
      if (!value) {
        return std::unexpected(std::move(value.error()));
      }
      parsed.trace_export_file_supplied = true;
      parsed.trace_export_file_path = std::move(*value);
      continue;
    }

    if (matches_flag(arg, "--trace-export-post")) {
      if (parsed.trace_export_post_supplied) {
        return std::unexpected(arg_error("--trace-export-post may be provided only once"));
      }
      auto value = consume_value(args, index, "--trace-export-post");
      if (!value) {
        return std::unexpected(std::move(value.error()));
      }
      parsed.trace_export_post_supplied = true;
      parsed.trace_export_post_url = std::move(*value);
      continue;
    }

    constexpr auto kConfigPrefix = std::string_view{"--config="};
    if (arg.starts_with(kConfigPrefix)) {
      if (parsed.has_explicit_config) {
        return std::unexpected(arg_error("--config may be provided only once"));
      }
      parsed.has_explicit_config = true;
      parsed.explicit_config_path = std::string{arg.substr(kConfigPrefix.size())};
      if (parsed.explicit_config_path.empty()) {
        return std::unexpected(arg_error("--config requires a non-empty path"));
      }
      continue;
    }

    if (arg == "--config") {
      if (parsed.has_explicit_config) {
        return std::unexpected(arg_error("--config may be provided only once"));
      }
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

  if (parsed.trace_export_limit_supplied && !parsed.trace_export) {
    return std::unexpected(arg_error("--limit requires --trace-export"));
  }
  if (parsed.trace_export_file_supplied && !parsed.trace_export) {
    return std::unexpected(arg_error("--trace-export-file requires --trace-export"));
  }
  if (parsed.trace_export_post_supplied && !parsed.trace_export) {
    return std::unexpected(arg_error("--trace-export-post requires --trace-export"));
  }
  if (parsed.trace_export_file_supplied && parsed.trace_export_post_supplied) {
    return std::unexpected(arg_error("--trace-export-file and --trace-export-post are mutually exclusive"));
  }
  if (parsed.trace_export && parsed.selector_mode_supplied) {
    return std::unexpected(arg_error("--mode is not supported with --trace-export"));
  }
  if (parsed.trace_export && !parsed.trace_turn_id_hex.empty() && parsed.selector_agent_supplied) {
    return std::unexpected(arg_error("--agent requires --trace-export list mode"));
  }
  if (parsed.trace_export && !parsed.trace_turn_id_hex.empty() && parsed.trace_export_limit_supplied) {
    return std::unexpected(arg_error("--limit requires --trace-export list mode"));
  }
  if (parsed.trace_export && !parsed.cli_args.empty()) {
    return std::unexpected(
        arg_error("--trace-export received an unsupported argument").with("arg", std::string{parsed.cli_args.front()}));
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

[[nodiscard]] std::string default_skills_directory(std::string_view workspace) {
  auto path = std::filesystem::path{std::string{workspace}};
  path /= kSkillsDirectoryRelative;
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
  std::println("usage: orangutan [--config <path>] [--mode <m>] [--agent <name>] [--explain-rules]");
  std::println("                  [--audit-init [<path>]] [--trace <turn-id>]");
  std::println("                  [--trace-export [<turn-id>] [--agent <name>] [--limit <n>]");
  std::println("                                  [--trace-export-file <path>|--trace-export-post <url>]]");
  std::println("                  [--desktop] [--serve] [--prompt <text>] [--help]");
  std::println();
  std::println(
      "The current bootstrap slice loads config, builds configured provider backends, then hands prompts to oran-cli.");
  std::println("--explain-rules prints the materialized permission rule set and exits;");
  std::println("                --mode picks the baseline (strict|default|permissive|sandboxed),");
  std::println("                --agent picks an `agents.<name>` overlay.");
  std::println("--mode/--agent also select configured provider-route prompt runs.");
  std::println("--audit-init applies the audit.db schema (defaults to <workspace>/.orangutan/audit.db) and exits.");
  std::println("--desktop opens the in-process Slint desktop app (requires a build configured with --desktop=y).");
  std::println("--serve runs the long-lived service: it auto-starts the IO file-view watcher and, when the");
  std::println("        config has automation.cron.jobs[], automation.triggered.jobs[], or an enabled");
  std::println("        automation.webhooks.listener, the automation loop plus the tool-scheduler idle-lock");
  std::println("        reaping tick; when the config has channels[], the channel ingress/dispatch loop. Runs until");
  std::println("        SIGINT/SIGTERM (exit code 128 + signum).");
  std::println("--trace prints the trace_turns row and joined audit rows, including hook_publish, for <turn-id>");
  std::println("        (32 lowercase hex characters); reads <workspace>/.orangutan/audit.db.");
  std::println("--trace-export prints trace rows plus joined audit rows as JSON Lines; a turn id prints one");
  std::println("               turn, otherwise --agent filters and --limit bounds the newest turns.");
  std::println("               --trace-export-file writes the same JSON Lines to a file instead of stdout.");
  std::println("               --trace-export-post POSTs the same JSON Lines to an HTTP endpoint.");
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

/// One-shot audit-DB operator command (`--audit-init`, `--trace`,
/// `--trace-export`): opens `audit_path` as a single-reader `Pool` on a fresh
/// io_context with `SignalScope` SIGINT/SIGTERM handling, runs the idempotent
/// audit migration, then drives `body(audit_repo, trace_repo, migration)` to
/// completion. The full `async::Runtime` thread pool exists for the agent
/// loop; a single io_context is the right shape for a one-shot operator
/// command. A delivered signal wins over the body outcome and maps to
/// `Error::cancelled` with `signal` / `signum` context.
template <typename T, typename Body>
[[nodiscard]] Result<T> run_audit_db_command(const std::string& audit_path, Body body) {
  asio::io_context io;
  SignalScope signals{io};

  auto pool_result =
      storage::Pool::open(io.get_executor(),
                          storage::PoolOptions{.path = audit_path, .reader_count = 1, .statement_cache_capacity = 4});
  if (!pool_result) {
    return std::unexpected(std::move(pool_result).error());
  }
  auto pool = std::move(*pool_result);
  storage::AuditRepository audit_repo{pool};
  storage::TraceRepository trace_repo{pool};

  auto outcome = std::optional<Result<T>>{};
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        auto migrated = co_await audit_repo.migrate();
        if (!migrated) {
          outcome = std::unexpected(std::move(migrated).error());
        } else {
          outcome = co_await body(audit_repo, trace_repo, *migrated);
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
  if (!outcome.has_value()) {
    return std::unexpected(Error::internal("audit db command did not complete"));
  }
  return std::move(*outcome);
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

  auto report = run_audit_db_command<storage::MigrationReport>(
      audit_path,
      [](storage::AuditRepository&, storage::TraceRepository&, const storage::MigrationReport& migration)
          -> async::Awaitable<Result<storage::MigrationReport>> { co_return migration; });
  if (!report) {
    return std::unexpected(std::move(report).error());
  }

  std::println("audit schema ready: version {} at {} ({} migrations applied)",
               report->current_version,
               audit_path,
               report->applied_versions.size());
  return 0;
}

/// Decode a 32-char lowercase hex string into a `core::TurnId`. The
/// inspector takes the operator-visible spelling produced by the storage
/// boundary's BLOB round-trip, so the only accepted shape is the same
/// lowercase hex that `core::format_turn_id_hex` emits. Rejects empty strings,
/// wrong-length inputs, uppercase / non-hex characters, and the all-zero
/// turn id (which `TraceRepository::append_turn` already rejects).
[[nodiscard]] Result<core::TurnId> parse_turn_id_hex(std::string_view text, std::string_view flag) {
  constexpr auto kExpectedSize = core::TurnId{}.size() * 2;
  if (text.size() != kExpectedSize) {
    return std::unexpected(arg_error(std::string{flag} + " turn id must be 32 lowercase hex characters")
                               .with("length", std::to_string(text.size())));
  }

  auto decode_nibble = [](char c) -> Result<unsigned char> {
    if (c >= '0' && c <= '9') {
      return static_cast<unsigned char>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
      return static_cast<unsigned char>(10 + (c - 'a'));
    }
    return std::unexpected(arg_error("turn id must be lowercase hex").with("char", std::string{1, c}));
  };

  core::TurnId id{};
  for (std::size_t i = 0; i < id.size(); ++i) {
    auto high = decode_nibble(text[i * 2]);
    if (!high) {
      return std::unexpected(std::move(high).error());
    }
    auto low = decode_nibble(text[i * 2 + 1]);
    if (!low) {
      return std::unexpected(std::move(low).error());
    }
    id[i] = static_cast<std::byte>(static_cast<unsigned char>((*high << 4) | *low));
  }
  if (core::is_zero_turn_id(id)) {
    return std::unexpected(arg_error(std::string{flag} + " turn id must not be all zero"));
  }
  return id;
}

[[nodiscard]] std::string format_u64_hex(std::uint64_t value) {
  return std::format("0x{:016x}", value);
}

struct TraceInspectRows {
  storage::TraceTurnRecord trace;
  std::vector<storage::AuditEventRecord> audits;
};

struct TraceExportListQuery {
  std::string agent_key{};
  std::size_t limit{kTraceExportDefaultLimit};
};

/// Shared read-only trace lookup for `--trace` and `--trace-export`.
/// Resolves the workspace audit DB, then loads the matching `trace_turns` row
/// plus every `audit_events` row whose `parent_turn_id` matches the turn id
/// through `run_audit_db_command` (whose idempotent migration lets the
/// inspector tolerate a fresh DB that has not yet seen `--audit-init`).
[[nodiscard]] Result<TraceInspectRows>
load_trace_inspect_rows(std::string_view workspace, std::string_view turn_id_hex, std::string_view flag) {
  if (workspace.empty()) {
    return std::unexpected(Error::invalid_argument("workspace path is empty"));
  }

  auto turn_id = parse_turn_id_hex(turn_id_hex, flag);
  if (!turn_id) {
    return std::unexpected(std::move(turn_id).error());
  }

  auto audit_path = default_audit_path(workspace);
  auto exists = path_exists(audit_path);
  if (!exists) {
    return std::unexpected(std::move(exists).error());
  }
  if (!*exists) {
    return std::unexpected(
        Error::not_found("audit database not found; run --audit-init first").with("path", audit_path));
  }

  return run_audit_db_command<TraceInspectRows>(
      audit_path,
      [&turn_id, turn_id_hex](storage::AuditRepository& audit_repo,
                              storage::TraceRepository& trace_repo,
                              const storage::MigrationReport&) -> async::Awaitable<Result<TraceInspectRows>> {
        auto trace = co_await trace_repo.get_turn(*turn_id);
        if (!trace) {
          co_return std::unexpected(std::move(trace).error());
        }
        if (!trace->has_value()) {
          co_return std::unexpected(Error::not_found("trace turn not found").with("turn_id", std::string{turn_id_hex}));
        }

        auto audits = co_await audit_repo.list_events_for_turn(*turn_id);
        if (!audits) {
          co_return std::unexpected(std::move(audits).error());
        }
        co_return TraceInspectRows{.trace = std::move(**trace), .audits = std::move(*audits)};
      });
}

[[nodiscard]] Result<std::vector<TraceInspectRows>> load_trace_export_list_rows(std::string_view workspace,
                                                                                const TraceExportListQuery& query) {
  if (workspace.empty()) {
    return std::unexpected(Error::invalid_argument("workspace path is empty"));
  }

  auto audit_path = default_audit_path(workspace);
  auto exists = path_exists(audit_path);
  if (!exists) {
    return std::unexpected(std::move(exists).error());
  }
  if (!*exists) {
    return std::unexpected(
        Error::not_found("audit database not found; run --audit-init first").with("path", audit_path));
  }

  return run_audit_db_command<std::vector<TraceInspectRows>>(
      audit_path,
      [&query](storage::AuditRepository& audit_repo,
               storage::TraceRepository& trace_repo,
               const storage::MigrationReport&) -> async::Awaitable<Result<std::vector<TraceInspectRows>>> {
        auto traces = co_await trace_repo.list_turns(
            storage::ListTraceTurnsOptions{.agent_key = query.agent_key, .limit = query.limit});
        if (!traces) {
          co_return std::unexpected(std::move(traces).error());
        }

        auto export_rows = std::vector<TraceInspectRows>{};
        export_rows.reserve(traces->size());
        for (auto& trace : *traces) {
          auto audits = co_await audit_repo.list_events_for_turn(trace.turn_id);
          if (!audits) {
            co_return std::unexpected(std::move(audits).error());
          }
          export_rows.push_back(TraceInspectRows{.trace = std::move(trace), .audits = std::move(*audits)});
        }
        co_return export_rows;
      });
}

[[nodiscard]] nlohmann::ordered_json parse_json_value_or_string(std::string_view text) {
  auto parsed = nlohmann::ordered_json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (parsed.is_discarded()) {
    return std::string{text};
  }
  return parsed;
}

[[nodiscard]] nlohmann::ordered_json optional_turn_id_json(const std::optional<core::TurnId>& id) {
  if (!id) {
    return nullptr;
  }
  return core::format_turn_id_hex(*id);
}

[[nodiscard]] nlohmann::ordered_json optional_string_json(const std::optional<std::string>& value) {
  if (!value) {
    return nullptr;
  }
  return *value;
}

[[nodiscard]] nlohmann::ordered_json trace_turn_json(const storage::TraceTurnRecord& row) {
  auto usage = nlohmann::ordered_json{
      {"cache_creation_tokens", row.cache_creation_tokens},
      {"cache_read_tokens", row.cache_read_tokens},
      {"input_tokens", row.input_tokens},
      {"output_tokens", row.output_tokens},
      {"cost_estimate_usd", row.cost_estimate_usd},
  };
  return nlohmann::ordered_json{
      {"turn_id", core::format_turn_id_hex(row.turn_id)},
      {"parent_turn_id", optional_turn_id_json(row.parent_turn_id)},
      {"session_id", core::format_turn_id_hex(row.session_id)},
      {"agent_key", row.agent_key},
      {"origin", row.origin},
      {"route_profile", row.route_profile},
      {"route_model", row.route_model},
      {"started_at_ns", row.started_at_ns},
      {"finished_at_ns", row.finished_at_ns},
      {"duration_ns", row.finished_at_ns - row.started_at_ns},
      {"stop_reason", row.stop_reason},
      {"iteration_count", row.iteration_count},
      {"prompt_prefix_hash", format_u64_hex(row.prompt_prefix_hash)},
      {"prompt_prefix_bytes", row.prompt_prefix_bytes},
      {"active_catalog_hash", format_u64_hex(row.active_catalog_hash)},
      {"deferred_catalog_hash", format_u64_hex(row.deferred_catalog_hash)},
      {"usage", std::move(usage)},
      {"cancellation_phase",
       row.cancellation_phase.has_value() ? nlohmann::ordered_json{*row.cancellation_phase}
                                          : nlohmann::ordered_json{nullptr}},
      {"schema_version", row.schema_version},
      {"context_json", parse_json_value_or_string(row.context_json)},
  };
}

[[nodiscard]] nlohmann::ordered_json audit_event_json(const storage::AuditEventRecord& audit) {
  return nlohmann::ordered_json{
      {"id", audit.id},
      {"event_kind", audit.event_kind},
      {"scope_key", audit.scope_key},
      {"agent_key", audit.agent_key},
      {"tool_name", audit.tool_name},
      {"identity", audit.identity},
      {"verdict", audit.verdict},
      {"outcome", audit.outcome},
      {"reason", audit.reason},
      {"input_hash_hex", optional_string_json(audit.input_hash_hex)},
      {"parent_turn_id", optional_turn_id_json(audit.parent_turn_id)},
      {"metadata_json", parse_json_value_or_string(audit.metadata_json)},
      {"created_at", audit.created_at},
  };
}

[[nodiscard]] nlohmann::ordered_json trace_export_json(const TraceInspectRows& rows) {
  auto audits = nlohmann::ordered_json::array();
  for (const auto& audit : rows.audits) {
    audits.push_back(audit_event_json(audit));
  }
  return nlohmann::ordered_json{
      {"kind", "trace_turn"},
      {"trace", trace_turn_json(rows.trace)},
      {"audit_rows", std::move(audits)},
  };
}

[[nodiscard]] Result<std::vector<std::string>> load_trace_export_lines(std::string_view workspace,
                                                                       const ParsedArgs& parsed) {
  if (!parsed.trace_turn_id_hex.empty()) {
    auto rows = load_trace_inspect_rows(workspace, parsed.trace_turn_id_hex, "--trace-export");
    if (!rows) {
      return std::unexpected(std::move(rows).error());
    }
    return std::vector<std::string>{trace_export_json(*rows).dump()};
  }

  auto rows = load_trace_export_list_rows(
      workspace,
      TraceExportListQuery{.agent_key = parsed.explain_selector.agent_name, .limit = parsed.trace_export_limit});
  if (!rows) {
    return std::unexpected(std::move(rows).error());
  }

  auto lines = std::vector<std::string>{};
  lines.reserve(rows->size());
  for (const auto& row : *rows) {
    lines.push_back(trace_export_json(row).dump());
  }
  return lines;
}

[[nodiscard]] std::string trace_export_jsonl_payload(const std::vector<std::string>& lines) {
  auto size = std::size_t{0};
  for (const auto& line : lines) {
    size += line.size() + 1;
  }

  auto payload = std::string{};
  payload.reserve(size);
  for (const auto& line : lines) {
    payload.append(line);
    payload.push_back('\n');
  }
  return payload;
}

[[nodiscard]] Result<void> write_trace_export_file(std::string_view path_text, const std::vector<std::string>& lines) {
  auto path = std::filesystem::path{std::string{path_text}};
  auto ec = std::error_code{};
  const auto parent = path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      return std::unexpected(Error::io("failed to create trace export file directory")
                                 .with("path", parent.string())
                                 .with("detail", ec.message()));
    }
  }

  auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
  if (!output) {
    return std::unexpected(Error::io("failed to open trace export file").with("path", path.string()));
  }
  output << trace_export_jsonl_payload(lines);
  output.flush();
  if (!output) {
    return std::unexpected(Error::io("failed to write trace export file").with("path", path.string()));
  }
  return {};
}

[[nodiscard]] Result<void> post_trace_export_lines(std::string_view url, const std::vector<std::string>& lines) {
  auto runtime = async::Runtime{async::RuntimeConfig{.io_workers = 1, .cpu_workers = 1}};
  auto client = http::Client{runtime.cpu_executor()};
  auto response = std::make_shared<std::optional<Result<http::BodyResponse>>>();

  asio::co_spawn(
      runtime.executor(),
      [&client, &runtime, response, url_text = std::string{url}, payload = trace_export_jsonl_payload(lines)]() mutable
          -> async::Awaitable<void> {
        auto request = http::BodyRequest{};
        request.method = "POST";
        request.url = std::move(url_text);
        request.headers = {
            http::Header{"content-type", "application/x-ndjson"},
            http::Header{"accept", "application/json, text/plain, */*"},
        };
        request.body = std::move(payload);
        request.timeout = std::chrono::milliseconds{30000};
        auto posted = co_await client.send(std::move(request));
        *response = std::move(posted);
        runtime.stop();
        co_return;
      },
      asio::detached);

  auto run_result = runtime.run();
  if (!run_result) {
    return std::unexpected(std::move(run_result).error());
  }
  if (!response->has_value()) {
    return std::unexpected(Error::internal("trace export POST did not complete"));
  }
  auto posted = std::move(**response);
  if (!posted) {
    return std::unexpected(std::move(posted).error());
  }
  if (posted->status_code < 200 || posted->status_code >= 300) {
    return std::unexpected(Error::io("trace export POST returned non-2xx status")
                               .with("status_code", std::to_string(posted->status_code)));
  }
  return {};
}

[[nodiscard]] Result<int> run_trace_inspect(std::string_view workspace, std::string_view turn_id_hex) {
  auto rows = load_trace_inspect_rows(workspace, turn_id_hex, "--trace");
  if (!rows) {
    return std::unexpected(std::move(rows).error());
  }

  const auto& row = rows->trace;
  std::println("trace turn {}:", core::format_turn_id_hex(row.turn_id));
  std::println("  session_id={} agent={} origin={}",
               core::format_turn_id_hex(row.session_id),
               row.agent_key,
               row.origin);
  std::println("  route={}/{} stop_reason={} iterations={}",
               row.route_profile,
               row.route_model,
               row.stop_reason,
               row.iteration_count);
  std::println("  started_at_ns={} finished_at_ns={} duration_ns={}",
               row.started_at_ns,
               row.finished_at_ns,
               row.finished_at_ns - row.started_at_ns);
  std::println("  prompt_prefix_hash=0x{:016x} bytes={} active_catalog=0x{:016x} deferred_catalog=0x{:016x}",
               row.prompt_prefix_hash,
               row.prompt_prefix_bytes,
               row.active_catalog_hash,
               row.deferred_catalog_hash);
  std::println("  usage: input={} output={} cache_creation={} cache_read={} cost_estimate_usd={}",
               row.input_tokens,
               row.output_tokens,
               row.cache_creation_tokens,
               row.cache_read_tokens,
               row.cost_estimate_usd);
  std::println("  cancellation_phase={}", row.cancellation_phase.value_or(std::string{"none"}));
  std::println("  parent_turn_id={}",
               row.parent_turn_id.has_value() ? core::format_turn_id_hex(*row.parent_turn_id) : std::string{"none"});
  std::println("  schema_version={} context_json_bytes={}", row.schema_version, row.context_json.size());

  std::println("audit rows: {}", rows->audits.size());
  std::size_t audit_index = 0;
  for (const auto& audit : rows->audits) {
    std::println("  #{:<3} kind={} verdict={} outcome={} tool={} scope={} agent={} identity={} reason={}",
                 audit_index,
                 audit.event_kind,
                 audit.verdict,
                 audit.outcome,
                 audit.tool_name,
                 audit.scope_key,
                 audit.agent_key,
                 audit.identity,
                 audit.reason);
    ++audit_index;
  }
  return 0;
}

[[nodiscard]] Result<int> run_trace_export(std::string_view workspace, const ParsedArgs& parsed) {
  auto lines = load_trace_export_lines(workspace, parsed);
  if (!lines) {
    return std::unexpected(std::move(lines).error());
  }

  if (parsed.trace_export_file_supplied) {
    auto written = write_trace_export_file(parsed.trace_export_file_path, *lines);
    if (!written) {
      return std::unexpected(std::move(written.error()));
    }
    return 0;
  }
  if (parsed.trace_export_post_supplied) {
    auto posted = post_trace_export_lines(parsed.trace_export_post_url, *lines);
    if (!posted) {
      return std::unexpected(std::move(posted.error()));
    }
    return 0;
  }
  for (const auto& line : *lines) {
    std::println("{}", line);
  }
  return 0;
}

[[nodiscard]] Result<std::optional<provider::AdapterConstructionPlan>>
resolve_default_provider_route(const config::Config& cfg) {
  if (cfg.routes().empty()) {
    return std::optional<provider::AdapterConstructionPlan>{};
  }
  auto resolution = provider::resolve_route_profiles(cfg, "default");
  if (!resolution) {
    return std::unexpected(std::move(resolution).error());
  }
  auto plan = provider::make_adapter_construction_plan(*resolution);
  if (!plan) {
    return std::unexpected(std::move(plan).error());
  }
  return std::optional<provider::AdapterConstructionPlan>{std::move(*plan)};
}

void print_provider_route_summary(const provider::AdapterConstructionPlan& route) {
  std::println("provider route: default primary={}/{} protocol={} fallbacks={}",
               route.primary.profile.target.profile,
               route.primary.profile.target.model,
               core::enum_name(route.primary.profile.target.protocol),
               route.fallbacks.size());
  for (std::size_t index = 0; index < route.fallbacks.size(); ++index) {
    const auto& fallback = route.fallbacks[index];
    std::println("  fallback #{}: {}/{} protocol={}",
                 index,
                 fallback.profile.target.profile,
                 fallback.profile.target.model,
                 core::enum_name(fallback.profile.target.protocol));
  }
}

// `loop_executor` must be the strand that also owns the runner's
// `ToolScheduler` (`AgentPromptRunnerOptions::executor`): the scheduler's
// lock table and per-batch cancellation signals are single-strand by
// contract, and the CLI turn coroutine is the `run_batch` caller whose
// phase-2 cancellation emits must serialize with the spawned tool calls.
[[nodiscard]] Result<int> run_cli_async_on_runtime(async::Runtime& runtime,
                                                   asio::any_io_executor loop_executor,
                                                   cli::CliOptions options,
                                                   cli::PromptRunner* runner) {
  auto result = std::make_shared<std::optional<core::Result<cli::CliResult>>>();
  asio::co_spawn(
      std::move(loop_executor),
      [options, runner, &runtime, result]() mutable -> async::Awaitable<void> {
        try {
          *result = co_await cli::run_async(options, runner);
        } catch (const std::exception& error) {
          *result = std::unexpected(Error::internal("CLI async handoff failed").with("reason", error.what()));
        } catch (...) {
          *result = std::unexpected(Error::internal("CLI async handoff failed").with("reason", "unknown"));
        }
        runtime.stop();
        co_return;
      },
      asio::detached);

  auto runtime_result = runtime.run();
  if (!runtime_result) {
    return std::unexpected(std::move(runtime_result).error());
  }
  if (!result->has_value()) {
    return std::unexpected(Error::internal("CLI async handoff did not complete"));
  }

  auto cli_result = std::move(**result);
  if (!cli_result) {
    return std::unexpected(std::move(cli_result).error());
  }
  return cli_result->exit_code;
}

#if defined(ORAN_ENABLE_DESKTOP)
// Scripted offline reply for the desktop demo fallback — used only when no
// provider route resolves, so `orangutan --desktop` still streams *something*
// (and `stop` is demonstrable during the per-turn latency window) without a
// configured model. A real route drives the live provider through the same
// runner. The plan supplies several identical turns so a short smoke session of
// a few prompts works before the fake reports "plan exhausted" (surfaced as an
// error line in the transcript).
[[nodiscard]] std::vector<provider::ScriptedTurn> desktop_demo_plan() {
  std::vector<provider::ScriptedTurn> plan;
  for (int turn = 0; turn < 16; ++turn) {
    plan.push_back(provider::ScriptedTurn{
        .response = std::nullopt,
        .deltas =
            {
                provider::TextDelta{.text = "No provider route is configured, so this is a scripted "},
                provider::TextDelta{.text = "offline reply. Add a route to your config to chat with a "},
                provider::TextDelta{.text = "real model."},
                provider::StreamEnd{.stop_reason = core::StopReason::end_turn,
                                    .usage = std::nullopt,
                                    .model_used = std::nullopt},
            },
        .error = std::nullopt,
        .latency = std::chrono::milliseconds{1500},
    });
  }
  return plan;
}

[[nodiscard]] provider::Route desktop_demo_route() {
  return provider::Route{
      .primary = provider::ModelTarget{.profile = "desktop-demo",
                                       .model = "scripted-1",
                                       .protocol = provider::ProtocolKind::anthropic_messages,
                                       .thinking_budget = std::nullopt,
                                       .cache = std::nullopt},
      .fallbacks = {},
  };
}

// `orangutan --desktop` launch: assemble the same runtime / provider / runner
// the CLI prompt path uses, but drive it from the desktop `ChatBridge` + Slint
// shell instead of the terminal REPL. The agent runs on the `async::Runtime`'s
// own workers (`Runtime::start`) while Slint owns the main thread; the bridge's
// lock-guarded channels cross between them. When no route resolves we fall back
// to a scripted `FakeProvider` so the window still demonstrates streaming + stop
// offline.
[[nodiscard]] Result<int> run_desktop(const BootstrapOptions& options) {
  auto loaded = load_config(options);
  if (!loaded) {
    return std::unexpected(std::move(loaded).error());
  }
  const auto& cfg = loaded->value;

  auto provider_route = resolve_default_provider_route(cfg);
  if (!provider_route) {
    return std::unexpected(std::move(provider_route).error());
  }

  auto runtime = async::Runtime{async::RuntimeConfig{
      .io_workers = static_cast<std::size_t>(std::max<std::int64_t>(1, cfg.runtime().workers)),
      .cpu_workers = 1,
  }};

  auto assembly_options = RuntimeAssemblyOptions{};
  assembly_options.workspace_options = tool::WorkspaceOptions{
      .extra_read_roots = cfg.permissions().workspace.extra_read_roots,
      .extra_write_roots = cfg.permissions().workspace.extra_write_roots,
  };
  assembly_options.trace_enabled = cfg.trace().enabled;
  assembly_options.hook_blocking_timeout = std::chrono::milliseconds{cfg.hooks().timeout_ms};
  assembly_options.session_memory_enabled = provider_route->has_value();
  assembly_options.longterm_memory_enabled = provider_route->has_value();
  auto assembly = RuntimeAssembly::build(options.workspace, runtime.executor(), std::move(assembly_options));
  if (!assembly) {
    return std::unexpected(std::move(assembly).error());
  }

  // One strand hosts the chat session loop, the runner-owned `ToolScheduler`
  // (single-strand lock table / cancellation contract), and the bridge whose
  // `request_stop` posts its per-turn cancellation emit — so a UI stop is
  // serialized with `begin_turn`'s signal replacement even with several io
  // workers. The bridge the UI and runtime share is built before the runner so
  // the runner borrows its sink (`event_sink`) as the streaming observer.
  auto agent_strand = runtime.make_strand();
  desktop::ChatBridge bridge{desktop::ChatBridgeOptions{.executor = agent_strand}};
  const bool dark = cfg.desktop().theme == "dark";

  // Shared launch body. Kept as a lambda so the live backend / scripted fake
  // each stay alive for the whole window lifetime (the runner borrows the
  // provider). The session coroutine must finish before the runner drops; the
  // explicit runtime join then proves no Runtime-owned worker can retain a
  // borrow into the runner/provider/assembly during teardown.
  auto launch = [&](provider::System& system, const provider::Route& route) -> Result<int> {
    auto runner = AgentPromptRunner::create(AgentPromptRunnerOptions{
        .executor = agent_strand,
        .assembly = &*assembly,
        .config = &cfg,
        .provider = &system,
        .route = route,
        .scope_key = "desktop",
        .agent_key = "default",
        .identity = "desktop",
        .origin = "desktop",
        .max_tokens = 1024,
        .quiet = true,
        .event_sink = bridge.event_sink(),
    });
    if (!runner) {
      return std::unexpected(std::move(runner).error());
    }

    desktop::TurnRunner turn_runner =
        [runner_ptr = runner->get()](std::string prompt,
                                     provider::EventSink* sink) -> async::Awaitable<core::Result<void>> {
      auto outcome = co_await runner_ptr->run_prompt(cli::PromptRunRequest{.prompt = std::move(prompt)});
      if (!outcome) {
        // Cancellation / provider error ended the turn without a terminal delta;
        // synthesize one so the view-model leaves the streaming state.
        if (sink != nullptr) {
          sink->on_done(core::StopReason::end_turn);
        }
        co_return std::unexpected(std::move(outcome).error());
      }
      co_return core::Result<void>{};
    };

    if (auto started = runtime.start(); !started) {
      return std::unexpected(std::move(started).error());
    }

    std::promise<void> session_done;
    auto session_done_future = session_done.get_future();
    asio::co_spawn(agent_strand,
                   desktop::run_chat_session(bridge, std::move(turn_runner)),
                   [done = std::move(session_done)](std::exception_ptr /*ep*/, core::Result<void> /*result*/) mutable {
                     done.set_value();
                   });

    auto code = desktop::shell::run(desktop::shell::RunOptions{
        .bridge = &bridge,
        .dark = dark,
        .reduce_motion = cfg.desktop().reduce_motion,
    });

    // Window closed: close the input (idempotent — the shell already did), cancel
    // any in-flight turn, wait for the session loop to finish (so the runner is no
    // longer borrowed), then stop the runtime.
    bridge.close();
    bridge.request_stop();
    session_done_future.wait();
    auto stopped = runtime.stop_and_join();
    if (!stopped) {
      return std::unexpected(std::move(stopped).error());
    }

    if (!code) {
      return std::unexpected(std::move(code).error());
    }
    return *code;
  };

  if (provider_route->has_value()) {
    auto backend =
        HttpProviderBackend::build(cfg,
                                   HttpProviderBackendOptions{
                                       .blocking_executor = runtime.cpu_executor(),
                                       .request_timeout = std::chrono::milliseconds{cfg.runtime().request_timeout_ms},
                                       .route_name = "default",
                                   });
    if (!backend) {
      return std::unexpected(std::move(backend).error());
    }
    return launch(backend->system(), backend->route());
  }

  provider::FakeProvider fake{desktop_demo_plan()};
  return launch(fake, desktop_demo_route());
}
#endif  // ORAN_ENABLE_DESKTOP

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
  // Operator commands surface SIGINT/SIGTERM as `Error::cancelled` with
  // signal context; report the interruption and map it to the conventional
  // `128 + signum` exit code instead of surfacing it as a failure.
  auto exit_code_for_interrupted = [](Result<int> result) -> Result<int> {
    if (!result && result.error().kind() == core::ErrorKind::cancelled) {
      if (auto signum = signum_from_error(result.error()); signum) {
        std::println(stderr, "orangutan: interrupted by {} ({})", signal_name(*signum), *signum);
        return 128 + *signum;
      }
    }
    return result;
  };

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
    return exit_code_for_interrupted(run_audit_init(std::move(audit_path)));
  }

  if (parsed->trace_inspect) {
    return exit_code_for_interrupted(run_trace_inspect(options.workspace, parsed->trace_turn_id_hex));
  }

  if (parsed->trace_export) {
    return exit_code_for_interrupted(run_trace_export(options.workspace, *parsed));
  }

  if (parsed->desktop) {
#if defined(ORAN_ENABLE_DESKTOP)
    return run_desktop(options);
#else
    return std::unexpected(arg_error("--desktop requires a build configured with --desktop=y (see docs/DESKTOP.md)"));
#endif
  }

  if (parsed->serve) {
    return exit_code_for_interrupted(run_serve(options));
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
  std::println("config summary: profiles={}, routes={}, workers={}, desktop={}",
               loaded->value.profiles().size(),
               loaded->value.routes().size(),
               loaded->value.runtime().workers,
               loaded->value.desktop().enabled ? "enabled" : "disabled");
  if (!loaded->value.warnings().empty()) {
    std::println("config warnings: {}", loaded->value.warnings().size());
  }

  auto provider_route = resolve_default_provider_route(loaded->value);
  if (!provider_route) {
    return std::unexpected(std::move(provider_route).error());
  }
  if (provider_route->has_value()) {
    print_provider_route_summary(**provider_route);
  } else {
    std::println("provider route: none configured");
    if (parsed->selector_mode_supplied || parsed->selector_agent_supplied) {
      return std::unexpected(arg_error("--mode/--agent require --explain-rules or a configured provider route"));
    }
  }

  if (provider_route->has_value()) {
    if (auto hybrid_search = validate_longterm_hybrid_search_policy(loaded->value); !hybrid_search) {
      return std::unexpected(std::move(hybrid_search.error()));
    }
  }
  auto longterm_hybrid_search = LongtermHybridSearchOptions{};
  if (provider_route->has_value()) {
    auto parsed_hybrid_search = longterm_hybrid_search_options_from(loaded->value);
    if (!parsed_hybrid_search) {
      return std::unexpected(std::move(parsed_hybrid_search.error()));
    }
    longterm_hybrid_search = std::move(*parsed_hybrid_search);
  }

  // The runtime assembly composes the per-process permission infrastructure
  // (`ApprovalBroker`, audit/trace storage, hook bus, and optional session
  // memory) plus the file-tool `tool::Workspace` inherited by agent-loop
  // prompt runs. Slice 41 routes
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
  assembly_options.trace_enabled = loaded->value.trace().enabled;
  assembly_options.trace_retention_started_before_ns =
      retention_started_before_ns(loaded->value.trace().retention_days);
  assembly_options.hook_blocking_timeout = std::chrono::milliseconds{loaded->value.hooks().timeout_ms};
  assembly_options.session_memory_enabled = provider_route->has_value();
  assembly_options.longterm_memory_enabled = provider_route->has_value();
  assembly_options.longterm_vector_memory_enabled = longterm_hybrid_search.enabled;
  assembly_options.longterm_vector_memory_dimensions = longterm_hybrid_search.embedding_dimensions;
  auto cron_jobs = cron_jobs_from(loaded->value);
  if (!cron_jobs) {
    return std::unexpected(std::move(cron_jobs.error()));
  }
  assembly_options.cron_jobs = std::move(*cron_jobs);
  auto triggered_jobs = triggered_jobs_from(loaded->value);
  if (!triggered_jobs) {
    return std::unexpected(std::move(triggered_jobs.error()));
  }
  assembly_options.triggered_jobs = std::move(*triggered_jobs);
  if (provider_route->has_value()) {
    const auto retention_now = core::time::now_utc();
    const auto retention_first_fire_at =
        core::Time{retention_now.to_system_time_point() +
                   std::chrono::hours{loaded->value.memory().longterm.retention.decay_check_interval_hours}};
    auto retention_job = longterm_memory_retention_job_from(loaded->value, "cli", retention_first_fire_at);
    if (!retention_job) {
      return std::unexpected(std::move(retention_job).error());
    }
    assembly_options.longterm_memory_startup_decay =
        longterm_memory_startup_decay_options_from(*retention_job, retention_now);
    assembly_options.longterm_memory_retention_job = std::move(*retention_job);
  }
  auto assembly = RuntimeAssembly::build(options.workspace, runtime.executor(), std::move(assembly_options));
  if (!assembly) {
    return std::unexpected(std::move(assembly).error());
  }
  std::println(
      "runtime assembly ready: audit={} ({}), approval-broker=fresh, workspace={}, trace={}, sessions={} ({}), "
      "longterm-memory={} ({}), startup-decay={}, vector-memory={} ({}), hook-timeout={}ms, automation-cron-jobs={}, "
      "automation-triggered-jobs={}",
      assembly->audit_enabled() ? "enabled" : "disabled",
      assembly->audit_enabled() ? assembly->audit_path() : std::string_view{"<null sink>"},
      assembly->workspace().root(),
      assembly->trace_enabled() ? "enabled" : "disabled",
      assembly->session_memory_enabled() ? "enabled" : "disabled",
      assembly->session_memory_enabled() ? assembly->sessions_path() : std::string_view{"<disabled>"},
      assembly->longterm_memory_enabled() ? "enabled" : "disabled",
      assembly->longterm_memory_enabled() ? assembly->longterm_memory_path() : std::string_view{"<disabled>"},
      longterm_startup_decay_summary(*assembly),
      assembly->longterm_vector_memory_enabled() ? "enabled" : "disabled",
      assembly->longterm_vector_memory_enabled() ? assembly->longterm_vector_memory_path()
                                                 : std::string_view{"<disabled>"},
      assembly->hook_bus().options().blocking_timeout.count(),
      assembly->cron_jobs().size(),
      assembly->triggered_jobs().size());

  if (!provider_route->has_value()) {
    auto cli_result = cli::run(cli::CliOptions{.args = std::span<const std::string_view>{parsed->cli_args}});
    if (!cli_result) {
      return std::unexpected(std::move(cli_result.error()));
    }
    return cli_result->exit_code;
  }

  auto provider_backend = HttpProviderBackend::build(
      loaded->value,
      HttpProviderBackendOptions{
          .blocking_executor = runtime.cpu_executor(),
          .request_timeout = std::chrono::milliseconds{loaded->value.runtime().request_timeout_ms},
          .route_name = "default",
      });
  if (!provider_backend) {
    return std::unexpected(std::move(provider_backend).error());
  }

  auto longterm_recall = longterm_recall_options_from(loaded->value);
  if (!longterm_recall) {
    return std::unexpected(std::move(longterm_recall.error()));
  }

  // One strand hosts the CLI agent loop and its runner-owned `ToolScheduler`
  // (whose lock table and batch cancellation signals are single-strand by
  // contract) while `runtime.workers` io workers still drive everything else.
  // Tool handlers and provider transport keep hopping to `cpu_executor()` for
  // blocking work, so bounded-parallel tool batches still overlap.
  auto agent_strand = runtime.make_strand();

  auto runner = AgentPromptRunner::create(AgentPromptRunnerOptions{
      .executor = agent_strand,
      .assembly = &*assembly,
      .config = &loaded->value,
      .provider = &provider_backend->system(),
      .route = provider_backend->route(),
      .mode = parsed->explain_selector.mode,
      .agent_config_name = parsed->explain_selector.agent_name,
      .permission_agent_name = parsed->explain_selector.agent_name,
      .scope_key = "cli",
      .agent_key =
          parsed->explain_selector.agent_name.empty() ? std::string{"default"} : parsed->explain_selector.agent_name,
      .identity = "terminal",
      .origin = "cli",
      .skills_directory = default_skills_directory(options.workspace),
      .longterm_recall = *longterm_recall,
      .longterm_hybrid_search = longterm_hybrid_search,
      .max_tokens = 1024,
  });
  if (!runner) {
    return std::unexpected(std::move(runner).error());
  }

  auto cli_result = run_cli_async_on_runtime(runtime,
                                             agent_strand,
                                             cli::CliOptions{
                                                 .args = std::span<const std::string_view>{parsed->cli_args},
                                                 .interactive_repl = true,
                                             },
                                             runner->get());
  if (!cli_result) {
    return std::unexpected(std::move(cli_result.error()));
  }
  return *cli_result;
}

}  // namespace orangutan::bootstrap
