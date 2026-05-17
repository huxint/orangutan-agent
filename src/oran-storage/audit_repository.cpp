// src/oran-storage/audit_repository.cpp — audit domain repository.

#include <oran/storage/audit_repository.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <oran/core/error.hpp>
#include <oran/storage/migrations.hpp>
#include <oran/storage/pool.hpp>
#include <oran/storage/sqlite.hpp>
#include <oran/storage/statement_cache.hpp>

namespace orangutan::storage {

namespace {

constexpr std::string_view kDefaultAuditMigrationsDirectory = "src/oran-storage/migrations/audit";

constexpr std::string_view kAppendEventSql = R"sql(
INSERT INTO audit_events(
  scope_key, agent_key, tool_name, identity, verdict, outcome, reason,
  input_hash_hex, metadata_json, created_at
)
VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
RETURNING id, created_at
)sql";

constexpr std::string_view kCountEventsSql = R"sql(
SELECT COUNT(*) FROM audit_events WHERE scope_key = ?
)sql";

[[nodiscard]] core::Error invalid_field(std::string field) {
  return core::Error::invalid_argument("audit repository field must not be empty").with("field", std::move(field));
}

[[nodiscard]] core::Result<void> validate_append_request(const AppendAuditEventRequest& request) {
  if (request.scope_key.empty()) {
    return std::unexpected(invalid_field("scope_key"));
  }
  if (request.agent_key.empty()) {
    return std::unexpected(invalid_field("agent_key"));
  }
  if (request.tool_name.empty()) {
    return std::unexpected(invalid_field("tool_name"));
  }
  if (request.identity.empty()) {
    return std::unexpected(invalid_field("identity"));
  }
  if (request.verdict.empty()) {
    return std::unexpected(invalid_field("verdict"));
  }
  if (request.outcome.empty()) {
    return std::unexpected(invalid_field("outcome"));
  }
  if (request.reason.empty()) {
    return std::unexpected(invalid_field("reason"));
  }
  if (request.metadata_json.empty()) {
    return std::unexpected(invalid_field("metadata_json"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_list_options(const ListAuditEventsOptions& options) {
  if (options.scope_key.empty()) {
    return std::unexpected(invalid_field("scope_key"));
  }
  if (options.limit == 0) {
    return std::unexpected(core::Error::invalid_argument("audit list limit must be greater than zero"));
  }
  if (options.limit > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
    return std::unexpected(core::Error::invalid_argument("audit list limit is too large"));
  }
  return {};
}

[[nodiscard]] core::Result<std::string> resolve_audit_migrations_directory(std::string_view configured_directory) {
  if (!configured_directory.empty()) {
    return std::string{configured_directory};
  }

  std::error_code ec;
  auto current = std::filesystem::current_path(ec);
  if (ec) {
    auto error = core::Error::io("failed to inspect current directory");
    error.with("system_error", ec.message());
    return std::unexpected(std::move(error));
  }

  while (true) {
    auto candidate = current / kDefaultAuditMigrationsDirectory;
    if (std::filesystem::is_directory(candidate, ec)) {
      return candidate.string();
    }
    if (ec) {
      ec.clear();
    }

    auto parent = current.parent_path();
    if (parent.empty() || parent == current) {
      break;
    }
    current = std::move(parent);
  }

  auto error = core::Error::not_found("default audit migrations directory does not exist");
  error.with("path", std::string{kDefaultAuditMigrationsDirectory});
  return std::unexpected(std::move(error));
}

[[nodiscard]] core::Result<std::string> required_text(Statement& statement, int index, std::string_view field) {
  auto value = statement.column_text(index);
  if (!value) {
    return std::unexpected(value.error().with("field", std::string{field}));
  }
  if (!*value) {
    return std::unexpected(
        core::Error::storage("audit repository row has null required field").with("field", std::string{field}));
  }
  return **std::move(value);
}

[[nodiscard]] core::Result<std::optional<std::string>> optional_text(Statement& statement, int index) {
  auto value = statement.column_text(index);
  if (!value) {
    return std::unexpected(value.error());
  }
  if (!*value) {
    return std::optional<std::string>{};
  }
  return std::optional<std::string>{**std::move(value)};
}

[[nodiscard]] core::Result<void> expect_done(Statement& statement, std::string_view operation) {
  auto done = statement.step();
  if (!done) {
    return std::unexpected(done.error());
  }
  if (*done != StepResult::done) {
    return std::unexpected(core::Error::storage("audit repository statement returned extra rows")
                               .with("operation", std::string{operation}));
  }
  return {};
}

[[nodiscard]] core::Result<AuditEventRecord> read_event_row(Statement& statement) {
  auto id = statement.column_int64(0);
  if (!id) {
    return std::unexpected(id.error().with("field", "id"));
  }
  auto scope_key = required_text(statement, 1, "scope_key");
  if (!scope_key) {
    return std::unexpected(scope_key.error());
  }
  auto agent_key = required_text(statement, 2, "agent_key");
  if (!agent_key) {
    return std::unexpected(agent_key.error());
  }
  auto tool_name = required_text(statement, 3, "tool_name");
  if (!tool_name) {
    return std::unexpected(tool_name.error());
  }
  auto identity = required_text(statement, 4, "identity");
  if (!identity) {
    return std::unexpected(identity.error());
  }
  auto verdict = required_text(statement, 5, "verdict");
  if (!verdict) {
    return std::unexpected(verdict.error());
  }
  auto outcome = required_text(statement, 6, "outcome");
  if (!outcome) {
    return std::unexpected(outcome.error());
  }
  auto reason = required_text(statement, 7, "reason");
  if (!reason) {
    return std::unexpected(reason.error());
  }
  auto input_hash = optional_text(statement, 8);
  if (!input_hash) {
    return std::unexpected(input_hash.error());
  }
  auto metadata_json = required_text(statement, 9, "metadata_json");
  if (!metadata_json) {
    return std::unexpected(metadata_json.error());
  }
  auto created_at = required_text(statement, 10, "created_at");
  if (!created_at) {
    return std::unexpected(created_at.error());
  }

  return AuditEventRecord{
      .id = *id,
      .scope_key = std::move(*scope_key),
      .agent_key = std::move(*agent_key),
      .tool_name = std::move(*tool_name),
      .identity = std::move(*identity),
      .verdict = std::move(*verdict),
      .outcome = std::move(*outcome),
      .reason = std::move(*reason),
      .input_hash_hex = std::move(*input_hash),
      .metadata_json = std::move(*metadata_json),
      .created_at = std::move(*created_at),
  };
}

/// Build the dynamic SELECT for `list_events`. The SQL grows secondary
/// filters as bind parameters so re-used statement cache entries can hit
/// for repeat callers with the same shape.
[[nodiscard]] std::string build_list_sql(const ListAuditEventsOptions& options) {
  std::string sql{"SELECT id, scope_key, agent_key, tool_name, identity, verdict, outcome, reason, "
                  "input_hash_hex, metadata_json, created_at "
                  "FROM audit_events WHERE scope_key = ?"};
  if (!options.agent_key.empty()) {
    sql += " AND agent_key = ?";
  }
  if (!options.tool_name.empty()) {
    sql += " AND tool_name = ?";
  }
  if (!options.outcome.empty()) {
    sql += " AND outcome = ?";
  }
  sql += " ORDER BY id DESC LIMIT ?";
  return sql;
}

}  // namespace

AuditRepository::AuditRepository(Pool& pool, AuditRepositoryOptions options) noexcept
    : pool_{&pool}, options_{std::move(options)} {}

async::Awaitable<core::Result<MigrationReport>> AuditRepository::migrate() {
  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(writer.error());
  }

  auto directory = resolve_audit_migrations_directory(options_.migrations_directory);
  if (!directory) {
    co_return std::unexpected(directory.error());
  }

  auto report = run_migrations_from_directory(writer->connection(), *directory);
  if (!report) {
    co_return std::unexpected(report.error());
  }
  co_return std::move(*report);
}

async::Awaitable<core::Result<AuditEventRecord>> AuditRepository::append_event(AppendAuditEventRequest request) {
  if (auto valid = validate_append_request(request); !valid) {
    co_return std::unexpected(valid.error());
  }

  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(writer.error());
  }

  auto cached = writer->statement_cache().acquire(writer->connection(), kAppendEventSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();

  if (auto bound = statement.bind_text(1, request.scope_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(2, request.agent_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(3, request.tool_name); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(4, request.identity); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(5, request.verdict); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(6, request.outcome); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(7, request.reason); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (request.input_hash_hex.empty()) {
    if (auto bound = statement.bind_null(8); !bound) {
      co_return std::unexpected(bound.error());
    }
  } else {
    if (auto bound = statement.bind_text(8, request.input_hash_hex); !bound) {
      co_return std::unexpected(bound.error());
    }
  }
  if (auto bound = statement.bind_text(9, request.metadata_json); !bound) {
    co_return std::unexpected(bound.error());
  }

  auto step = statement.step();
  if (!step) {
    co_return std::unexpected(step.error());
  }
  if (*step != StepResult::row) {
    co_return std::unexpected(core::Error::storage("audit event insert returned no row"));
  }

  auto id = statement.column_int64(0);
  if (!id) {
    co_return std::unexpected(id.error().with("field", "id"));
  }
  auto created_at = required_text(statement, 1, "created_at");
  if (!created_at) {
    co_return std::unexpected(created_at.error());
  }
  if (auto done = expect_done(statement, "append_event"); !done) {
    co_return std::unexpected(done.error());
  }

  auto record = AuditEventRecord{
      .id = *id,
      .scope_key = std::move(request.scope_key),
      .agent_key = std::move(request.agent_key),
      .tool_name = std::move(request.tool_name),
      .identity = std::move(request.identity),
      .verdict = std::move(request.verdict),
      .outcome = std::move(request.outcome),
      .reason = std::move(request.reason),
      .input_hash_hex = {},
      .metadata_json = std::move(request.metadata_json),
      .created_at = std::move(*created_at),
  };
  if (!request.input_hash_hex.empty()) {
    record.input_hash_hex = std::move(request.input_hash_hex);
  }
  co_return record;
}

async::Awaitable<core::Result<std::vector<AuditEventRecord>>>
AuditRepository::list_events(ListAuditEventsOptions options) {
  if (auto valid = validate_list_options(options); !valid) {
    co_return std::unexpected(valid.error());
  }

  auto reader = co_await pool_->acquire_reader();
  if (!reader) {
    co_return std::unexpected(reader.error());
  }

  const auto sql = build_list_sql(options);
  auto cached = reader->statement_cache().acquire(reader->connection(), sql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();

  int index = 1;
  if (auto bound = statement.bind_text(index++, options.scope_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (!options.agent_key.empty()) {
    if (auto bound = statement.bind_text(index++, options.agent_key); !bound) {
      co_return std::unexpected(bound.error());
    }
  }
  if (!options.tool_name.empty()) {
    if (auto bound = statement.bind_text(index++, options.tool_name); !bound) {
      co_return std::unexpected(bound.error());
    }
  }
  if (!options.outcome.empty()) {
    if (auto bound = statement.bind_text(index++, options.outcome); !bound) {
      co_return std::unexpected(bound.error());
    }
  }
  if (auto bound = statement.bind_int64(index, static_cast<std::int64_t>(options.limit)); !bound) {
    co_return std::unexpected(bound.error());
  }

  std::vector<AuditEventRecord> events;
  while (true) {
    auto step = statement.step();
    if (!step) {
      co_return std::unexpected(step.error());
    }
    if (*step == StepResult::done) {
      break;
    }
    auto event = read_event_row(statement);
    if (!event) {
      co_return std::unexpected(event.error());
    }
    events.push_back(std::move(*event));
  }

  co_return events;
}

async::Awaitable<core::Result<std::int64_t>> AuditRepository::count_events(std::string scope_key) {
  if (scope_key.empty()) {
    co_return std::unexpected(invalid_field("scope_key"));
  }

  auto reader = co_await pool_->acquire_reader();
  if (!reader) {
    co_return std::unexpected(reader.error());
  }

  auto cached = reader->statement_cache().acquire(reader->connection(), kCountEventsSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_text(1, scope_key); !bound) {
    co_return std::unexpected(bound.error());
  }

  auto step = statement.step();
  if (!step) {
    co_return std::unexpected(step.error());
  }
  if (*step != StepResult::row) {
    co_return std::unexpected(core::Error::storage("audit event count returned no row"));
  }

  auto count = statement.column_int64(0);
  if (!count) {
    co_return std::unexpected(count.error().with("field", "count"));
  }
  if (auto done = expect_done(statement, "count_events"); !done) {
    co_return std::unexpected(done.error());
  }
  co_return *count;
}

}  // namespace orangutan::storage
