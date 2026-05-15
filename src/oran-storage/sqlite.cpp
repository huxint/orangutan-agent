// src/oran-storage/sqlite.cpp — expected-only SQLite core implementation.

#include <oran/storage/sqlite.hpp>

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <sqlite3.h>

#include <oran/core/error.hpp>

namespace orangutan::storage {

namespace {

struct DbHandle {
  explicit DbHandle(sqlite3* ptr) noexcept : db{ptr} {}

  ~DbHandle() {
    if (db != nullptr) {
      sqlite3_close(db);
    }
  }

  DbHandle(const DbHandle&) = delete;
  DbHandle& operator=(const DbHandle&) = delete;

  sqlite3* db{};
};

[[nodiscard]] core::Error sqlite_error(sqlite3* db, std::string message, std::string_view sql = {}) {
  const auto code = db == nullptr ? SQLITE_ERROR : sqlite3_errcode(db);
  const auto extended_code = db == nullptr ? SQLITE_ERROR : sqlite3_extended_errcode(db);
  const auto sqlite_message = db == nullptr ? "sqlite handle unavailable" : sqlite3_errmsg(db);
  auto error = core::Error::storage(std::move(message))
                   .with("sqlite_code", std::to_string(code))
                   .with("sqlite_extended_code", std::to_string(extended_code))
                   .with("sqlite_message", sqlite_message);
  if (!sql.empty()) {
    error.with("sql", std::string{sql});
  }
  return error;
}

[[nodiscard]] core::Error closed_error() {
  return core::Error{core::ErrorKind::conflict, "sqlite connection is closed"};
}

[[nodiscard]] core::Error invalid_statement_error() {
  return core::Error{core::ErrorKind::conflict, "sqlite statement is not valid"};
}

[[nodiscard]] core::Error no_current_row_error() {
  return core::Error{core::ErrorKind::conflict, "sqlite statement is not positioned on a row"};
}

[[nodiscard]] core::Error invalid_column_error(int index) {
  return core::Error::invalid_argument("column index is out of range").with("index", std::to_string(index));
}

[[nodiscard]] int open_flags(OpenMode mode) noexcept {
  switch (mode) {
    case OpenMode::read_only:
      return SQLITE_OPEN_READONLY;
    case OpenMode::read_write:
      return SQLITE_OPEN_READWRITE;
    case OpenMode::read_write_create:
      return SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
  }
  return SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
}

[[nodiscard]] bool is_in_memory_path(std::string_view path) noexcept {
  return path == ":memory:";
}

[[nodiscard]] bool has_handle(const std::shared_ptr<DbHandle>& handle) noexcept {
  return handle && handle->db != nullptr;
}

[[nodiscard]] core::Result<void> check_bind_result(sqlite3* db, int rc, std::string_view operation) {
  if (rc != SQLITE_OK) {
    return std::unexpected(sqlite_error(db, std::string{operation}));
  }
  return {};
}

[[nodiscard]] std::string first_column_text_or_missing(const QueryResult& result) {
  if (result.rows.empty() || result.rows.front().values.empty() || !result.rows.front().values.front()) {
    return "<missing>";
  }
  return *result.rows.front().values.front();
}

}  // namespace

struct Statement::Impl {
  Impl(std::shared_ptr<DbHandle> h, sqlite3_stmt* s) noexcept : handle{std::move(h)}, stmt{s} {}

  ~Impl() {
    if (stmt != nullptr) {
      sqlite3_finalize(stmt);
    }
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  std::shared_ptr<DbHandle> handle;
  sqlite3_stmt* stmt{};
  bool has_current_row{false};
};

struct Connection::Impl {
  explicit Impl(std::shared_ptr<DbHandle> h) noexcept : handle{std::move(h)} {}

  std::shared_ptr<DbHandle> handle;
};

Statement::Statement() = default;

Statement::Statement(std::unique_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}

Statement::~Statement() = default;

Statement::Statement(Statement&&) noexcept = default;

Statement& Statement::operator=(Statement&&) noexcept = default;

bool Statement::valid() const noexcept {
  return impl_ && impl_->stmt != nullptr && has_handle(impl_->handle);
}

core::Result<void> Statement::bind_null(int index) {
  if (!valid()) {
    return std::unexpected(invalid_statement_error());
  }
  return check_bind_result(impl_->handle->db, sqlite3_bind_null(impl_->stmt, index), "sqlite bind null failed");
}

core::Result<void> Statement::bind_int64(int index, std::int64_t value) {
  if (!valid()) {
    return std::unexpected(invalid_statement_error());
  }
  return check_bind_result(impl_->handle->db,
                           sqlite3_bind_int64(impl_->stmt, index, value),
                           "sqlite bind int64 failed");
}

core::Result<void> Statement::bind_double(int index, double value) {
  if (!valid()) {
    return std::unexpected(invalid_statement_error());
  }
  return check_bind_result(impl_->handle->db,
                           sqlite3_bind_double(impl_->stmt, index, value),
                           "sqlite bind double failed");
}

core::Result<void> Statement::bind_text(int index, std::string_view value) {
  if (!valid()) {
    return std::unexpected(invalid_statement_error());
  }
  const auto rc = sqlite3_bind_text(impl_->stmt, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
  return check_bind_result(impl_->handle->db, rc, "sqlite bind text failed");
}

core::Result<void> Statement::clear_bindings() {
  if (!valid()) {
    return std::unexpected(invalid_statement_error());
  }
  return check_bind_result(impl_->handle->db, sqlite3_clear_bindings(impl_->stmt), "sqlite clear bindings failed");
}

core::Result<void> Statement::reset() {
  if (!valid()) {
    return std::unexpected(invalid_statement_error());
  }
  impl_->has_current_row = false;
  const auto rc = sqlite3_reset(impl_->stmt);
  if (rc != SQLITE_OK) {
    return std::unexpected(sqlite_error(impl_->handle->db, "sqlite statement reset failed"));
  }
  return {};
}

core::Result<StepResult> Statement::step() {
  if (!valid()) {
    return std::unexpected(invalid_statement_error());
  }

  const auto rc = sqlite3_step(impl_->stmt);
  if (rc == SQLITE_ROW) {
    impl_->has_current_row = true;
    return StepResult::row;
  }
  impl_->has_current_row = false;
  if (rc == SQLITE_DONE) {
    return StepResult::done;
  }
  return std::unexpected(sqlite_error(impl_->handle->db, "sqlite statement step failed"));
}

core::Result<int> Statement::column_count() const {
  if (!valid()) {
    return std::unexpected(invalid_statement_error());
  }
  return sqlite3_column_count(impl_->stmt);
}

core::Result<std::string> Statement::column_name(int index) const {
  if (!valid()) {
    return std::unexpected(invalid_statement_error());
  }
  const auto count = sqlite3_column_count(impl_->stmt);
  if (index < 0 || index >= count) {
    return std::unexpected(invalid_column_error(index));
  }
  const char* name = sqlite3_column_name(impl_->stmt, index);
  return name == nullptr ? std::string{} : std::string{name};
}

core::Result<ColumnValue> Statement::column_text(int index) const {
  if (!valid()) {
    return std::unexpected(invalid_statement_error());
  }
  if (!impl_->has_current_row) {
    return std::unexpected(no_current_row_error());
  }
  const auto count = sqlite3_column_count(impl_->stmt);
  if (index < 0 || index >= count) {
    return std::unexpected(invalid_column_error(index));
  }
  if (sqlite3_column_type(impl_->stmt, index) == SQLITE_NULL) {
    return ColumnValue{};
  }

  const auto* text = sqlite3_column_text(impl_->stmt, index);
  const auto bytes = sqlite3_column_bytes(impl_->stmt, index);
  if (text == nullptr) {
    return ColumnValue{std::string{}};
  }
  return ColumnValue{std::string{reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes)}};
}

core::Result<std::int64_t> Statement::column_int64(int index) const {
  if (!valid()) {
    return std::unexpected(invalid_statement_error());
  }
  if (!impl_->has_current_row) {
    return std::unexpected(no_current_row_error());
  }
  const auto count = sqlite3_column_count(impl_->stmt);
  if (index < 0 || index >= count) {
    return std::unexpected(invalid_column_error(index));
  }
  return sqlite3_column_int64(impl_->stmt, index);
}

core::Result<double> Statement::column_double(int index) const {
  if (!valid()) {
    return std::unexpected(invalid_statement_error());
  }
  if (!impl_->has_current_row) {
    return std::unexpected(no_current_row_error());
  }
  const auto count = sqlite3_column_count(impl_->stmt);
  if (index < 0 || index >= count) {
    return std::unexpected(invalid_column_error(index));
  }
  return sqlite3_column_double(impl_->stmt, index);
}

Connection::Connection() = default;

Connection::Connection(std::unique_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}

Connection::~Connection() = default;

Connection::Connection(Connection&&) noexcept = default;

Connection& Connection::operator=(Connection&&) noexcept = default;

core::Result<Connection> Connection::open(ConnectionOptions options) {
  if (options.path.empty()) {
    return std::unexpected(core::Error::invalid_argument("sqlite path must not be empty"));
  }

  sqlite3* raw = nullptr;
  const auto flags = open_flags(options.mode) | SQLITE_OPEN_NOMUTEX;
  const auto rc = sqlite3_open_v2(options.path.c_str(), &raw, flags, nullptr);
  std::shared_ptr<DbHandle> handle{std::make_shared<DbHandle>(raw)};
  if (rc != SQLITE_OK) {
    return std::unexpected(sqlite_error(raw, "sqlite open failed").with("path", options.path));
  }

  sqlite3_extended_result_codes(handle->db, 1);

  if (options.busy_timeout_ms > 0) {
    const auto timeout_rc = sqlite3_busy_timeout(handle->db, options.busy_timeout_ms);
    if (timeout_rc != SQLITE_OK) {
      return std::unexpected(sqlite_error(handle->db, "sqlite busy timeout setup failed").with("path", options.path));
    }
  }

  Connection connection{std::make_unique<Impl>(std::move(handle))};
  if (options.enforce_foreign_keys) {
    if (auto foreign_keys = connection.execute("PRAGMA foreign_keys = ON"); !foreign_keys) {
      return std::unexpected(foreign_keys.error().with("path", options.path));
    }
  }
  if (options.enable_wal && options.mode != OpenMode::read_only && !is_in_memory_path(options.path)) {
    auto wal = connection.query("PRAGMA journal_mode = WAL");
    if (!wal) {
      return std::unexpected(wal.error().with("path", options.path));
    }
    const auto journal_mode = first_column_text_or_missing(*wal);
    if (journal_mode != "wal") {
      return std::unexpected(core::Error::storage("sqlite WAL setup did not enter WAL mode")
                                 .with("path", options.path)
                                 .with("journal_mode", journal_mode));
    }
  }

  return connection;
}

bool Connection::is_open() const noexcept {
  return impl_ && has_handle(impl_->handle);
}

void Connection::close() noexcept {
  impl_.reset();
}

core::Result<void> Connection::execute(std::string_view sql) {
  if (!is_open()) {
    return std::unexpected(closed_error());
  }
  if (sql.empty()) {
    return std::unexpected(core::Error::invalid_argument("sql must not be empty"));
  }

  char* raw_message = nullptr;
  const auto rc = sqlite3_exec(impl_->handle->db, std::string{sql}.c_str(), nullptr, nullptr, &raw_message);
  if (rc != SQLITE_OK) {
    std::string message = raw_message == nullptr ? "sqlite execute failed" : raw_message;
    sqlite3_free(raw_message);
    return std::unexpected(sqlite_error(impl_->handle->db, std::move(message), sql));
  }
  return {};
}

core::Result<Statement> Connection::prepare(std::string_view sql) {
  if (!is_open()) {
    return std::unexpected(closed_error());
  }
  if (sql.empty()) {
    return std::unexpected(core::Error::invalid_argument("sql must not be empty"));
  }

  sqlite3_stmt* stmt = nullptr;
  std::string owned_sql{sql};
  const auto rc =
      sqlite3_prepare_v2(impl_->handle->db, owned_sql.c_str(), static_cast<int>(owned_sql.size()), &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return std::unexpected(sqlite_error(impl_->handle->db, "sqlite prepare failed", sql));
  }
  return Statement{std::make_unique<Statement::Impl>(impl_->handle, stmt)};
}

core::Result<QueryResult> Connection::query(std::string_view sql) {
  auto prepared = prepare(sql);
  if (!prepared) {
    return std::unexpected(prepared.error());
  }

  auto statement = std::move(*prepared);
  auto count = statement.column_count();
  if (!count) {
    return std::unexpected(count.error());
  }

  QueryResult result;
  result.columns.reserve(static_cast<std::size_t>(*count));
  for (int i = 0; i < *count; ++i) {
    auto name = statement.column_name(i);
    if (!name) {
      return std::unexpected(name.error());
    }
    result.columns.push_back(std::move(*name));
  }

  while (true) {
    auto step_result = statement.step();
    if (!step_result) {
      return std::unexpected(step_result.error());
    }
    if (*step_result == StepResult::done) {
      break;
    }

    Row row;
    row.values.reserve(static_cast<std::size_t>(*count));
    for (int i = 0; i < *count; ++i) {
      auto value = statement.column_text(i);
      if (!value) {
        return std::unexpected(value.error());
      }
      row.values.push_back(std::move(*value));
    }
    result.rows.push_back(std::move(row));
  }

  return result;
}

}  // namespace orangutan::storage
