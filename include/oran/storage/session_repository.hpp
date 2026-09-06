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

struct SessionMessageInput {
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

struct UpsertSessionSkillActivationRequest {
  std::string session_id;
  std::string agent_key;
  std::string skill_name;
  bool active{true};
};

struct SessionSkillActivationRecord {
  std::string session_id;
  std::string agent_key;
  std::string skill_name;
  bool active{true};
  std::string created_at;
  std::string updated_at;
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

  /// Appends an ordered suffix in one transaction. An empty suffix has no effect.
  [[nodiscard]] async::Awaitable<core::Result<std::vector<SessionMessageRecord>>>
  append_messages(SessionKey key, std::vector<SessionMessageInput> messages);

  [[nodiscard]] async::Awaitable<core::Result<std::vector<SessionMessageRecord>>> load_messages(SessionKey key);
  /// Latest rows in conversation order, bounded by encoded bytes and count.
  /// Stops before a row that would exceed the byte budget; stored history is unchanged.
  [[nodiscard]] async::Awaitable<core::Result<std::vector<SessionMessageRecord>>>
  load_tail(SessionKey key, std::size_t max_messages = 128, std::size_t max_bytes = 512 * 1024);

  [[nodiscard]] async::Awaitable<core::Result<SessionSkillActivationRecord>>
  upsert_skill_activation(UpsertSessionSkillActivationRequest request);

  [[nodiscard]] async::Awaitable<core::Result<std::vector<SessionSkillActivationRecord>>>
  load_skill_activations(SessionKey key);

  [[nodiscard]] async::Awaitable<core::Result<std::optional<SessionRecord>>> get_session(SessionKey key);

  [[nodiscard]] async::Awaitable<core::Result<std::vector<SessionRecord>>> list_sessions(ListSessionsOptions options);

private:
  Pool* pool_{};
  SessionRepositoryOptions options_;
};

}  // namespace orangutan::storage
