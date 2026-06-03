// src/oran-storage/session_repository.cpp — sessions domain repository.

#include <oran/storage/session_repository.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <oran/core/error.hpp>
#include <oran/core/role.hpp>
#include <oran/storage/migrations.hpp>
#include <oran/storage/pool.hpp>
#include <oran/storage/sqlite.hpp>
#include <oran/storage/statement_cache.hpp>

namespace orangutan::storage {

namespace {

constexpr std::string_view kAppendMessageSql = R"sql(
INSERT INTO session_messages(session_id, agent_key, sequence, role, content_json, metadata_json, created_at)
VALUES (
  ?, ?,
  (
    SELECT COALESCE(MAX(sequence), 0) + 1
    FROM session_messages
    WHERE session_id = ? AND agent_key = ?
  ),
  ?, ?, ?,
  strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
)
RETURNING sequence, created_at
)sql";

constexpr std::string_view kLoadMessagesSql = R"sql(
SELECT session_id, agent_key, sequence, role, content_json, metadata_json, created_at
FROM session_messages
WHERE session_id = ? AND agent_key = ?
ORDER BY sequence ASC
)sql";

constexpr std::string_view kTouchSessionSql = R"sql(
INSERT INTO sessions(session_id, agent_key, created_at, updated_at)
VALUES (?, ?, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
ON CONFLICT(session_id, agent_key) DO UPDATE SET updated_at = excluded.updated_at
)sql";

constexpr std::string_view kUpsertSkillActivationSql = R"sql(
INSERT INTO session_skill_activations(session_id, agent_key, skill_name, active, created_at, updated_at)
VALUES (?, ?, ?, ?, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
ON CONFLICT(session_id, agent_key, skill_name) DO UPDATE SET
  active = excluded.active,
  updated_at = excluded.updated_at
RETURNING session_id, agent_key, skill_name, active, created_at, updated_at
)sql";

constexpr std::string_view kLoadSkillActivationsSql = R"sql(
SELECT session_id, agent_key, skill_name, active, created_at, updated_at
FROM session_skill_activations
WHERE session_id = ? AND agent_key = ?
ORDER BY skill_name ASC
)sql";

constexpr std::string_view kGetSessionSql = R"sql(
SELECT s.session_id,
       s.agent_key,
       s.title,
       s.metadata_json,
       s.created_at,
       s.updated_at,
       COUNT(m.sequence) AS message_count
FROM sessions AS s
LEFT JOIN session_messages AS m
  ON m.session_id = s.session_id AND m.agent_key = s.agent_key
WHERE s.session_id = ? AND s.agent_key = ?
GROUP BY s.session_id, s.agent_key, s.title, s.metadata_json, s.created_at, s.updated_at
)sql";

constexpr std::string_view kListSessionsSql = R"sql(
SELECT s.session_id,
       s.agent_key,
       s.title,
       s.metadata_json,
       s.created_at,
       s.updated_at,
       COUNT(m.sequence) AS message_count
FROM sessions AS s
LEFT JOIN session_messages AS m
  ON m.session_id = s.session_id AND m.agent_key = s.agent_key
WHERE s.agent_key = ?
GROUP BY s.session_id, s.agent_key, s.title, s.metadata_json, s.created_at, s.updated_at
ORDER BY s.updated_at DESC, s.session_id ASC
LIMIT ?
)sql";

[[nodiscard]] core::Error invalid_field(std::string field) {
  return core::Error::invalid_argument("session repository field must not be empty").with("field", std::move(field));
}

[[nodiscard]] core::Result<void> validate_key(const SessionKey& key) {
  if (key.session_id.empty()) {
    return std::unexpected(invalid_field("session_id"));
  }
  if (key.agent_key.empty()) {
    return std::unexpected(invalid_field("agent_key"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_append_request(const AppendSessionMessageRequest& request) {
  if (auto valid = validate_key(SessionKey{.session_id = request.session_id, .agent_key = request.agent_key}); !valid) {
    return std::unexpected(valid.error());
  }
  if (request.content_json.empty()) {
    return std::unexpected(invalid_field("content_json"));
  }
  if (request.metadata_json.empty()) {
    return std::unexpected(invalid_field("metadata_json"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_skill_activation_request(const UpsertSessionSkillActivationRequest& request) {
  if (auto valid = validate_key(SessionKey{.session_id = request.session_id, .agent_key = request.agent_key}); !valid) {
    return std::unexpected(valid.error());
  }
  if (request.skill_name.empty()) {
    return std::unexpected(invalid_field("skill_name"));
  }
  return {};
}

[[nodiscard]] core::Result<std::int64_t> checked_limit(std::size_t limit) {
  if (limit == 0) {
    return std::unexpected(core::Error::invalid_argument("session list limit must be greater than zero"));
  }
  if (limit > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
    return std::unexpected(core::Error::invalid_argument("session list limit is too large"));
  }
  return static_cast<std::int64_t>(limit);
}

[[nodiscard]] core::Result<std::string> required_text(Statement& statement, int index, std::string_view field) {
  auto value = statement.column_text(index);
  if (!value) {
    return std::unexpected(value.error().with("field", std::string{field}));
  }
  if (!*value) {
    return std::unexpected(
        core::Error::storage("session repository row has null required field").with("field", std::string{field}));
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
    return std::unexpected(core::Error::storage("session repository statement returned extra rows")
                               .with("operation", std::string{operation}));
  }
  return {};
}

[[nodiscard]] core::Result<SessionMessageRecord> read_message_row(Statement& statement) {
  auto session_id = required_text(statement, 0, "session_id");
  if (!session_id) {
    return std::unexpected(session_id.error());
  }
  auto agent_key = required_text(statement, 1, "agent_key");
  if (!agent_key) {
    return std::unexpected(agent_key.error());
  }
  auto sequence = statement.column_int64(2);
  if (!sequence) {
    return std::unexpected(sequence.error().with("field", "sequence"));
  }
  auto role_text = required_text(statement, 3, "role");
  if (!role_text) {
    return std::unexpected(role_text.error());
  }
  auto role = core::parse_enum<core::Role>(*role_text);
  if (!role) {
    return std::unexpected(
        core::Error::storage("session repository row has unknown role").with("role", std::move(*role_text)));
  }
  auto content_json = required_text(statement, 4, "content_json");
  if (!content_json) {
    return std::unexpected(content_json.error());
  }
  auto metadata_json = required_text(statement, 5, "metadata_json");
  if (!metadata_json) {
    return std::unexpected(metadata_json.error());
  }
  auto created_at = required_text(statement, 6, "created_at");
  if (!created_at) {
    return std::unexpected(created_at.error());
  }

  return SessionMessageRecord{
      .session_id = std::move(*session_id),
      .agent_key = std::move(*agent_key),
      .sequence = *sequence,
      .role = *role,
      .content_json = std::move(*content_json),
      .metadata_json = std::move(*metadata_json),
      .created_at = std::move(*created_at),
  };
}

[[nodiscard]] core::Result<SessionSkillActivationRecord> read_skill_activation_row(Statement& statement) {
  auto session_id = required_text(statement, 0, "session_id");
  if (!session_id) {
    return std::unexpected(session_id.error());
  }
  auto agent_key = required_text(statement, 1, "agent_key");
  if (!agent_key) {
    return std::unexpected(agent_key.error());
  }
  auto skill_name = required_text(statement, 2, "skill_name");
  if (!skill_name) {
    return std::unexpected(skill_name.error());
  }
  auto active = statement.column_int64(3);
  if (!active) {
    return std::unexpected(active.error().with("field", "active"));
  }
  if (*active != 0 && *active != 1) {
    return std::unexpected(core::Error::storage("session skill activation row has invalid active value")
                               .with("active", std::to_string(*active)));
  }
  auto created_at = required_text(statement, 4, "created_at");
  if (!created_at) {
    return std::unexpected(created_at.error());
  }
  auto updated_at = required_text(statement, 5, "updated_at");
  if (!updated_at) {
    return std::unexpected(updated_at.error());
  }

  return SessionSkillActivationRecord{
      .session_id = std::move(*session_id),
      .agent_key = std::move(*agent_key),
      .skill_name = std::move(*skill_name),
      .active = *active == 1,
      .created_at = std::move(*created_at),
      .updated_at = std::move(*updated_at),
  };
}

[[nodiscard]] core::Result<SessionRecord> read_session_row(Statement& statement) {
  auto session_id = required_text(statement, 0, "session_id");
  if (!session_id) {
    return std::unexpected(session_id.error());
  }
  auto agent_key = required_text(statement, 1, "agent_key");
  if (!agent_key) {
    return std::unexpected(agent_key.error());
  }
  auto title = optional_text(statement, 2);
  if (!title) {
    return std::unexpected(title.error());
  }
  auto metadata_json = required_text(statement, 3, "metadata_json");
  if (!metadata_json) {
    return std::unexpected(metadata_json.error());
  }
  auto created_at = required_text(statement, 4, "created_at");
  if (!created_at) {
    return std::unexpected(created_at.error());
  }
  auto updated_at = required_text(statement, 5, "updated_at");
  if (!updated_at) {
    return std::unexpected(updated_at.error());
  }
  auto message_count = statement.column_int64(6);
  if (!message_count) {
    return std::unexpected(message_count.error().with("field", "message_count"));
  }

  return SessionRecord{
      .session_id = std::move(*session_id),
      .agent_key = std::move(*agent_key),
      .title = std::move(*title),
      .metadata_json = std::move(*metadata_json),
      .created_at = std::move(*created_at),
      .updated_at = std::move(*updated_at),
      .message_count = *message_count,
  };
}

}  // namespace

SessionRepository::SessionRepository(Pool& pool, SessionRepositoryOptions options) noexcept
    : pool_{&pool}, options_{std::move(options)} {}

async::Awaitable<core::Result<MigrationReport>> SessionRepository::migrate() {
  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(writer.error());
  }

  if (options_.migrations_directory.empty()) {
    auto report = run_migrations(writer->connection(), built_in_session_migrations());
    if (!report) {
      co_return std::unexpected(report.error());
    }
    co_return std::move(*report);
  }

  auto report = run_migrations_from_directory(writer->connection(), options_.migrations_directory);
  if (!report) {
    co_return std::unexpected(report.error());
  }
  co_return std::move(*report);
}

async::Awaitable<core::Result<SessionMessageRecord>>
SessionRepository::append_message(AppendSessionMessageRequest request) {
  if (auto valid = validate_append_request(request); !valid) {
    co_return std::unexpected(valid.error());
  }

  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(writer.error());
  }

  auto cached = writer->statement_cache().acquire(writer->connection(), kAppendMessageSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();

  if (auto bound = statement.bind_text(1, request.session_id); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(2, request.agent_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(3, request.session_id); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(4, request.agent_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(5, core::enum_name(request.role)); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(6, request.content_json); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(7, request.metadata_json); !bound) {
    co_return std::unexpected(bound.error());
  }

  auto step = statement.step();
  if (!step) {
    co_return std::unexpected(step.error());
  }
  if (*step != StepResult::row) {
    co_return std::unexpected(core::Error::storage("session message insert returned no row"));
  }

  auto sequence = statement.column_int64(0);
  if (!sequence) {
    co_return std::unexpected(sequence.error().with("field", "sequence"));
  }
  auto created_at = required_text(statement, 1, "created_at");
  if (!created_at) {
    co_return std::unexpected(created_at.error());
  }
  if (auto done = expect_done(statement, "append_message"); !done) {
    co_return std::unexpected(done.error());
  }

  co_return SessionMessageRecord{
      .session_id = std::move(request.session_id),
      .agent_key = std::move(request.agent_key),
      .sequence = *sequence,
      .role = request.role,
      .content_json = std::move(request.content_json),
      .metadata_json = std::move(request.metadata_json),
      .created_at = std::move(*created_at),
  };
}

async::Awaitable<core::Result<std::vector<SessionMessageRecord>>> SessionRepository::load_messages(SessionKey key) {
  if (auto valid = validate_key(key); !valid) {
    co_return std::unexpected(valid.error());
  }

  auto reader = co_await pool_->acquire_reader();
  if (!reader) {
    co_return std::unexpected(reader.error());
  }

  auto cached = reader->statement_cache().acquire(reader->connection(), kLoadMessagesSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_text(1, key.session_id); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(2, key.agent_key); !bound) {
    co_return std::unexpected(bound.error());
  }

  std::vector<SessionMessageRecord> messages;
  while (true) {
    auto step = statement.step();
    if (!step) {
      co_return std::unexpected(step.error());
    }
    if (*step == StepResult::done) {
      break;
    }
    auto message = read_message_row(statement);
    if (!message) {
      co_return std::unexpected(message.error());
    }
    messages.push_back(std::move(*message));
  }

  co_return messages;
}

async::Awaitable<core::Result<SessionSkillActivationRecord>>
SessionRepository::upsert_skill_activation(UpsertSessionSkillActivationRequest request) {
  if (auto valid = validate_skill_activation_request(request); !valid) {
    co_return std::unexpected(valid.error());
  }

  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(writer.error());
  }

  {
    auto cached = writer->statement_cache().acquire(writer->connection(), kTouchSessionSql);
    if (!cached) {
      co_return std::unexpected(cached.error());
    }
    auto& statement = cached->statement();
    if (auto bound = statement.bind_text(1, request.session_id); !bound) {
      co_return std::unexpected(bound.error());
    }
    if (auto bound = statement.bind_text(2, request.agent_key); !bound) {
      co_return std::unexpected(bound.error());
    }
    if (auto done = expect_done(statement, "touch_session_for_skill_activation"); !done) {
      co_return std::unexpected(done.error());
    }
  }

  auto cached = writer->statement_cache().acquire(writer->connection(), kUpsertSkillActivationSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_text(1, request.session_id); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(2, request.agent_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(3, request.skill_name); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_int64(4, request.active ? 1 : 0); !bound) {
    co_return std::unexpected(bound.error());
  }

  auto step = statement.step();
  if (!step) {
    co_return std::unexpected(step.error());
  }
  if (*step != StepResult::row) {
    co_return std::unexpected(core::Error::storage("session skill activation upsert returned no row"));
  }

  auto record = read_skill_activation_row(statement);
  if (!record) {
    co_return std::unexpected(record.error());
  }
  if (auto done = expect_done(statement, "upsert_skill_activation"); !done) {
    co_return std::unexpected(done.error());
  }
  co_return std::move(*record);
}

async::Awaitable<core::Result<std::vector<SessionSkillActivationRecord>>>
SessionRepository::load_skill_activations(SessionKey key) {
  if (auto valid = validate_key(key); !valid) {
    co_return std::unexpected(valid.error());
  }

  auto reader = co_await pool_->acquire_reader();
  if (!reader) {
    co_return std::unexpected(reader.error());
  }

  auto cached = reader->statement_cache().acquire(reader->connection(), kLoadSkillActivationsSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_text(1, key.session_id); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(2, key.agent_key); !bound) {
    co_return std::unexpected(bound.error());
  }

  std::vector<SessionSkillActivationRecord> records;
  while (true) {
    auto step = statement.step();
    if (!step) {
      co_return std::unexpected(step.error());
    }
    if (*step == StepResult::done) {
      break;
    }
    auto record = read_skill_activation_row(statement);
    if (!record) {
      co_return std::unexpected(record.error());
    }
    records.push_back(std::move(*record));
  }

  co_return records;
}

async::Awaitable<core::Result<std::optional<SessionRecord>>> SessionRepository::get_session(SessionKey key) {
  if (auto valid = validate_key(key); !valid) {
    co_return std::unexpected(valid.error());
  }

  auto reader = co_await pool_->acquire_reader();
  if (!reader) {
    co_return std::unexpected(reader.error());
  }

  auto cached = reader->statement_cache().acquire(reader->connection(), kGetSessionSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_text(1, key.session_id); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_text(2, key.agent_key); !bound) {
    co_return std::unexpected(bound.error());
  }

  auto step = statement.step();
  if (!step) {
    co_return std::unexpected(step.error());
  }
  if (*step == StepResult::done) {
    co_return std::optional<SessionRecord>{};
  }

  auto session = read_session_row(statement);
  if (!session) {
    co_return std::unexpected(session.error());
  }
  if (auto done = expect_done(statement, "get_session"); !done) {
    co_return std::unexpected(done.error());
  }
  co_return std::optional<SessionRecord>{std::move(*session)};
}

async::Awaitable<core::Result<std::vector<SessionRecord>>>
SessionRepository::list_sessions(ListSessionsOptions options) {
  if (options.agent_key.empty()) {
    co_return std::unexpected(invalid_field("agent_key"));
  }
  auto limit = checked_limit(options.limit);
  if (!limit) {
    co_return std::unexpected(limit.error());
  }

  auto reader = co_await pool_->acquire_reader();
  if (!reader) {
    co_return std::unexpected(reader.error());
  }

  auto cached = reader->statement_cache().acquire(reader->connection(), kListSessionsSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_text(1, options.agent_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (auto bound = statement.bind_int64(2, *limit); !bound) {
    co_return std::unexpected(bound.error());
  }

  std::vector<SessionRecord> sessions;
  while (true) {
    auto step = statement.step();
    if (!step) {
      co_return std::unexpected(step.error());
    }
    if (*step == StepResult::done) {
      break;
    }
    auto session = read_session_row(statement);
    if (!session) {
      co_return std::unexpected(session.error());
    }
    sessions.push_back(std::move(*session));
  }

  co_return sessions;
}

}  // namespace orangutan::storage
