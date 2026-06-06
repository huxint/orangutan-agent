// include/oran/storage/sqlite.hpp — expected-only SQLite core.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <oran/core/result.hpp>

namespace orangutan::storage {

using SqliteExtensionInit = void (*)();

enum class OpenMode : std::uint8_t {
  read_only,
  read_write,
  read_write_create,
};

struct ConnectionOptions {
  std::string path;
  OpenMode mode{OpenMode::read_write_create};
  int busy_timeout_ms{5000};
  bool enable_wal{true};
  bool enforce_foreign_keys{true};
};

enum class StepResult : std::uint8_t {
  row,
  done,
};

using ColumnValue = std::optional<std::string>;
using BlobValue = std::optional<std::vector<std::byte>>;

struct Row {
  std::vector<ColumnValue> values;
};

struct QueryResult {
  std::vector<std::string> columns;
  std::vector<Row> rows;
};

class Statement {
public:
  Statement();
  ~Statement();

  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  Statement(Statement&&) noexcept;
  Statement& operator=(Statement&&) noexcept;

  [[nodiscard]] bool valid() const noexcept;

  [[nodiscard]] core::Result<void> bind_null(int index);
  [[nodiscard]] core::Result<void> bind_int64(int index, std::int64_t value);
  [[nodiscard]] core::Result<void> bind_double(int index, double value);
  [[nodiscard]] core::Result<void> bind_text(int index, std::string_view value);
  [[nodiscard]] core::Result<void> bind_blob(int index, std::span<const std::byte> value);
  [[nodiscard]] core::Result<void> clear_bindings();
  [[nodiscard]] core::Result<void> reset();

  [[nodiscard]] core::Result<StepResult> step();

  [[nodiscard]] core::Result<int> column_count() const;
  [[nodiscard]] core::Result<std::string> column_name(int index) const;
  [[nodiscard]] core::Result<ColumnValue> column_text(int index) const;
  [[nodiscard]] core::Result<BlobValue> column_blob(int index) const;
  [[nodiscard]] core::Result<std::int64_t> column_int64(int index) const;
  [[nodiscard]] core::Result<double> column_double(int index) const;

private:
  struct Impl;
  explicit Statement(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;

  friend class Connection;
};

class Connection {
public:
  Connection();
  ~Connection();

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  Connection(Connection&&) noexcept;
  Connection& operator=(Connection&&) noexcept;

  [[nodiscard]] static core::Result<Connection> open(ConnectionOptions options,
                                                     std::span<const SqliteExtensionInit> auto_extensions = {});

  [[nodiscard]] bool is_open() const noexcept;
  void close() noexcept;

  [[nodiscard]] core::Result<void> execute(std::string_view sql);
  [[nodiscard]] core::Result<Statement> prepare(std::string_view sql);
  [[nodiscard]] core::Result<QueryResult> query(std::string_view sql);

private:
  struct Impl;
  explicit Connection(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

}  // namespace orangutan::storage
