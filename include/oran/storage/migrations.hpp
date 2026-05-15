// include/oran/storage/migrations.hpp — expected-only SQLite migrations.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <oran/core/result.hpp>

namespace orangutan::storage {

class Connection;

struct Migration {
  std::int64_t version{};
  std::string name;
  std::string sql;
};

struct MigrationReport {
  std::int64_t previous_version{};
  std::int64_t current_version{};
  std::vector<std::int64_t> applied_versions;
};

[[nodiscard]] core::Result<MigrationReport> run_migrations(Connection& connection,
                                                           std::span<const Migration> migrations);

}  // namespace orangutan::storage
