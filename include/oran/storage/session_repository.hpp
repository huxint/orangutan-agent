// include/oran/storage/session_repository.hpp — sessions domain repository.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/core/role.hpp>
#include <oran/storage/migrations.hpp>

namespace orangutan::storage {

class Pool;

struct SessionKey {
  std::string session_id;
  std::string agent_key;
};

struct AppendSessionMessageRequest {
  std::string session_id;
  std::string agent_key;
  core::Role role{core::Role::user};
  std::string content_json;
  std::string metadata_json{"{}"};
};

struct SessionMessageRecord {
  std::string session_id;
  std::string agent_key;
  std::int64_t sequence{};
  core::Role role{core::Role::user};
  std::string content_json;
  std::string metadata_json;
  std::string created_at;
};

struct SessionRecord {
  std::string session_id;
  std::string agent_key;
  std::optional<std::string> title;
  std::string metadata_json;
  std::string created_at;
  std::string updated_at;
  std::int64_t message_count{};
};

struct ListSessionsOptions {
  std::string agent_key;
  std::size_t limit{50};
};

struct SessionRepositoryOptions {
  std::string migrations_directory;
};

class SessionRepository {
public:
  explicit SessionRepository(Pool& pool, SessionRepositoryOptions options = {}) noexcept;

  [[nodiscard]] async::Awaitable<core::Result<MigrationReport>> migrate();

  [[nodiscard]] async::Awaitable<core::Result<SessionMessageRecord>>
  append_message(AppendSessionMessageRequest request);

  [[nodiscard]] async::Awaitable<core::Result<std::vector<SessionMessageRecord>>> load_messages(SessionKey key);

  [[nodiscard]] async::Awaitable<core::Result<std::optional<SessionRecord>>> get_session(SessionKey key);

  [[nodiscard]] async::Awaitable<core::Result<std::vector<SessionRecord>>> list_sessions(ListSessionsOptions options);

private:
  Pool* pool_{};
  SessionRepositoryOptions options_;
};

}  // namespace orangutan::storage
