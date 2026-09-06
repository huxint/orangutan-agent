#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/message.hpp>
#include <oran/core/result.hpp>

namespace orangutan::storage {
class SessionRepository;
}  // namespace orangutan::storage

namespace orangutan::memory::session {

struct SessionId {
  std::string value;

  friend bool operator==(const SessionId&, const SessionId&) = default;
};

struct AgentKey {
  std::string value;

  friend bool operator==(const AgentKey&, const AgentKey&) = default;
};

struct ListSessionsOptions {
  AgentKey agent_key;
  std::size_t limit{50};

  friend bool operator==(const ListSessionsOptions&, const ListSessionsOptions&) = default;
};

struct SessionSummary {
  SessionId session_id;
  AgentKey agent_key;
  std::size_t message_count{};
  std::string created_at;
  std::string updated_at;

  friend bool operator==(const SessionSummary&, const SessionSummary&) = default;
};

struct SkillActivationUpdate {
  std::string name;
  bool active{true};

  friend bool operator==(const SkillActivationUpdate&, const SkillActivationUpdate&) = default;
};

struct SkillActivationRecord {
  std::string name;
  bool active{true};
  std::string created_at;
  std::string updated_at;

  friend bool operator==(const SkillActivationRecord&, const SkillActivationRecord&) = default;
};

class Store {
public:
  explicit Store(storage::SessionRepository& repository) noexcept;

  [[nodiscard]] async::Awaitable<core::Result<void>>
  append(SessionId session_id, AgentKey agent_key, core::Message message);

  /// Serializes the whole suffix before writing it atomically. The caller retains
  /// the messages until completion; an empty suffix has no effect.
  [[nodiscard]] async::Awaitable<core::Result<void>>
  append_all(SessionId session_id, AgentKey agent_key, std::span<const core::Message> messages);

  [[nodiscard]] async::Awaitable<core::Result<std::vector<core::Message>>> load(SessionId session_id,
                                                                                AgentKey agent_key);
  [[nodiscard]] async::Awaitable<core::Result<std::vector<core::Message>>> load_tail(SessionId session_id,
                                                                                     AgentKey agent_key,
                                                                                     std::size_t max_messages = 128,
                                                                                     std::size_t max_bytes = 512 *
                                                                                                             1024);

  [[nodiscard]] async::Awaitable<core::Result<void>>
  record_skill_activation(SessionId session_id, AgentKey agent_key, SkillActivationUpdate update);

  [[nodiscard]] async::Awaitable<core::Result<std::vector<SkillActivationRecord>>>
  load_skill_activations(SessionId session_id, AgentKey agent_key);

  [[nodiscard]] async::Awaitable<core::Result<std::vector<SessionSummary>>> list(ListSessionsOptions options);

private:
  storage::SessionRepository* repository_{};
};

}  // namespace orangutan::memory::session
