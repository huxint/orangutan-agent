// src/oran-storage/migrations.cpp — SQLite migration runner.

#include <oran/storage/migrations.hpp>

#include <charconv>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <oran/core/error.hpp>
#include <oran/storage/sqlite.hpp>

namespace orangutan::storage {

namespace {

constexpr std::string_view kCreateSchemaVersionsSql = R"sql(
CREATE TABLE IF NOT EXISTS schema_versions(
  version INTEGER PRIMARY KEY,
  name TEXT NOT NULL,
  applied_at TEXT NOT NULL
)
)sql";

constexpr std::string_view kReadSchemaVersionsSql = "SELECT version FROM schema_versions ORDER BY version";

constexpr std::string_view kInsertSchemaVersionSql = R"sql(
INSERT INTO schema_versions(version, name, applied_at)
VALUES (?, ?, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
)sql";

[[nodiscard]] core::Error attach_migration_context(core::Error error, const Migration& migration) {
  error.with("migration_version", std::to_string(migration.version)).with("migration_name", migration.name);
  return error;
}

[[nodiscard]] core::Error rollback_error(core::Error error, const Migration& migration, Connection& connection) {
  if (auto rollback = connection.execute("ROLLBACK"); !rollback) {
    error.with("rollback_error", std::string{rollback.error().message()});
  }
  return attach_migration_context(std::move(error), migration);
}

[[nodiscard]] core::Result<void> validate_migration_list(std::span<const Migration> migrations) {
  std::int64_t expected_version = 1;
  for (const auto& migration : migrations) {
    if (migration.version <= 0) {
      return std::unexpected(core::Error::invalid_argument("migration version must be positive")
                                 .with("migration_version", std::to_string(migration.version)));
    }
    if (migration.version != expected_version) {
      return std::unexpected(core::Error::invalid_argument("migration versions must be contiguous from 1")
                                 .with("expected_version", std::to_string(expected_version))
                                 .with("actual_version", std::to_string(migration.version)));
    }
    if (migration.name.empty()) {
      return std::unexpected(core::Error::invalid_argument("migration name must not be empty")
                                 .with("migration_version", std::to_string(migration.version)));
    }
    if (migration.sql.empty()) {
      return std::unexpected(core::Error::invalid_argument("migration sql must not be empty")
                                 .with("migration_version", std::to_string(migration.version))
                                 .with("migration_name", migration.name));
    }
    ++expected_version;
  }
  return {};
}

[[nodiscard]] core::Result<std::int64_t> parse_version(std::string_view text) {
  std::int64_t version{};
  const auto* first = text.data();
  const auto* last = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(first, last, version);
  if (ec != std::errc{} || ptr != last) {
    return std::unexpected(
        core::Error::storage("schema_versions contains an invalid version").with("version_text", std::string{text}));
  }
  return version;
}

[[nodiscard]] core::Result<std::vector<std::int64_t>> read_applied_versions(Connection& connection) {
  auto rows = connection.query(kReadSchemaVersionsSql);
  if (!rows) {
    return std::unexpected(rows.error());
  }

  std::vector<std::int64_t> versions;
  versions.reserve(rows->rows.size());
  for (const auto& row : rows->rows) {
    if (row.values.empty() || !row.values.front()) {
      return std::unexpected(core::Error::storage("schema_versions row is missing version"));
    }
    auto version = parse_version(*row.values.front());
    if (!version) {
      return std::unexpected(version.error());
    }
    versions.push_back(*version);
  }
  return versions;
}

[[nodiscard]] core::Result<std::int64_t> validate_applied_versions(std::span<const std::int64_t> versions,
                                                                   std::size_t migration_count) {
  std::int64_t expected_version = 1;
  for (const auto version : versions) {
    if (version != expected_version) {
      return std::unexpected(core::Error{core::ErrorKind::conflict, "schema_versions is not contiguous"}
                                 .with("expected_version", std::to_string(expected_version))
                                 .with("actual_version", std::to_string(version)));
    }
    ++expected_version;
  }

  const auto current_version = versions.empty() ? 0 : versions.back();
  if (current_version > static_cast<std::int64_t>(migration_count)) {
    return std::unexpected(core::Error{core::ErrorKind::conflict, "database schema is newer than migration set"}
                               .with("current_version", std::to_string(current_version))
                               .with("migration_count", std::to_string(migration_count)));
  }
  return current_version;
}

[[nodiscard]] core::Result<void> record_migration(Connection& connection, const Migration& migration) {
  auto insert = connection.prepare(kInsertSchemaVersionSql);
  if (!insert) {
    return std::unexpected(insert.error());
  }
  if (auto bound = insert->bind_int64(1, migration.version); !bound) {
    return std::unexpected(bound.error());
  }
  if (auto bound = insert->bind_text(2, migration.name); !bound) {
    return std::unexpected(bound.error());
  }
  auto stepped = insert->step();
  if (!stepped) {
    return std::unexpected(stepped.error());
  }
  if (*stepped != StepResult::done) {
    return std::unexpected(core::Error::storage("schema version insert returned a row"));
  }
  return {};
}

[[nodiscard]] core::Result<void> apply_migration(Connection& connection, const Migration& migration) {
  if (auto begun = connection.execute("BEGIN IMMEDIATE"); !begun) {
    return std::unexpected(attach_migration_context(begun.error(), migration));
  }

  if (auto applied = connection.execute(migration.sql); !applied) {
    return std::unexpected(rollback_error(applied.error(), migration, connection));
  }

  if (auto recorded = record_migration(connection, migration); !recorded) {
    return std::unexpected(rollback_error(recorded.error(), migration, connection));
  }

  if (auto committed = connection.execute("COMMIT"); !committed) {
    return std::unexpected(rollback_error(committed.error(), migration, connection));
  }
  return {};
}

}  // namespace

core::Result<MigrationReport> run_migrations(Connection& connection, std::span<const Migration> migrations) {
  if (auto valid = validate_migration_list(migrations); !valid) {
    return std::unexpected(valid.error());
  }

  if (auto schema = connection.execute(kCreateSchemaVersionsSql); !schema) {
    return std::unexpected(schema.error());
  }

  auto applied_versions = read_applied_versions(connection);
  if (!applied_versions) {
    return std::unexpected(applied_versions.error());
  }

  auto current_version = validate_applied_versions(std::span<const std::int64_t>{*applied_versions}, migrations.size());
  if (!current_version) {
    return std::unexpected(current_version.error());
  }

  MigrationReport report{
      .previous_version = *current_version,
      .current_version = *current_version,
      .applied_versions = {},
  };

  for (const auto& migration : migrations) {
    if (migration.version <= report.current_version) {
      continue;
    }
    if (auto applied = apply_migration(connection, migration); !applied) {
      return std::unexpected(applied.error());
    }
    report.current_version = migration.version;
    report.applied_versions.push_back(migration.version);
  }

  return report;
}

}  // namespace orangutan::storage
