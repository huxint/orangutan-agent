// src/oran-memory/session.cpp — typed session memory over SessionRepository.

#include <oran/memory/session.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/core/time.hpp>
#include <oran/storage/session_repository.hpp>

namespace orangutan::memory::session {
namespace {

using json = nlohmann::ordered_json;

[[nodiscard]] core::Error invalid_field(std::string field) {
  return core::Error::invalid_argument("memory session field must not be empty").with("field", std::move(field));
}

[[nodiscard]] core::Result<void> validate_key(std::string_view session_id, std::string_view agent_key) {
  if (session_id.empty()) {
    return std::unexpected(invalid_field("session_id"));
  }
  if (agent_key.empty()) {
    return std::unexpected(invalid_field("agent_key"));
  }
  return {};
}

[[nodiscard]] bool is_blank(std::string_view value) noexcept {
  for (const auto ch : value) {
    if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool contains_control_char(std::string_view value) noexcept {
  for (const auto ch : value) {
    if (static_cast<unsigned char>(ch) < 0x20U) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] core::Result<void> validate_skill_activation_update(const SkillActivationUpdate& update) {
  if (is_blank(update.name)) {
    return std::unexpected(invalid_field("skill_name"));
  }
  if (contains_control_char(update.name)) {
    return std::unexpected(
        core::Error::invalid_argument("memory session skill activation name must not contain control characters")
            .with("skill", update.name));
  }
  return {};
}

[[nodiscard]] core::Error message_parse_error(std::string reason) {
  return core::Error::parsing("memory session message JSON is malformed").with("reason", std::move(reason));
}

[[nodiscard]] core::Result<std::size_t> checked_size(std::int64_t value, std::string field) {
  if (value < 0) {
    return std::unexpected(core::Error::storage("memory session repository returned negative count")
                               .with("field", std::move(field))
                               .with("value", std::to_string(value)));
  }
  if (static_cast<std::uint64_t>(value) > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return std::unexpected(core::Error::storage("memory session repository count exceeds platform size range")
                               .with("field", std::move(field)));
  }
  return static_cast<std::size_t>(value);
}

void put_if_present(json& object, std::string_view key, const std::optional<std::string>& value) {
  if (value.has_value()) {
    object[std::string{key}] = *value;
  }
}

[[nodiscard]] json content_to_json(const core::Content& content) {
  return std::visit(
      [](const auto& block) -> json {
        using T = std::decay_t<decltype(block)>;
        if constexpr (std::same_as<T, core::TextContent>) {
          return json{{"type", "text"}, {"text", block.text}};
        } else if constexpr (std::same_as<T, core::ThinkingContent>) {
          auto out = json{{"type", "thinking"}, {"thinking", block.thinking}};
          put_if_present(out, "signature", block.signature);
          return out;
        } else if constexpr (std::same_as<T, core::ToolUseContent>) {
          return json{{"type", "tool_use"}, {"id", block.id}, {"name", block.name}, {"input_json", block.input_json}};
        } else {
          auto out = json{{"type", "tool_result"},
                          {"tool_use_id", block.tool_use_id},
                          {"output", block.output},
                          {"is_error", block.is_error}};
          put_if_present(out, "data_json", block.data_json);
          return out;
        }
      },
      content);
}

[[nodiscard]] std::string message_to_json(const core::Message& message) {
  auto blocks = json::array();
  for (const auto& block : message.blocks) {
    blocks.push_back(content_to_json(block));
  }

  auto out = json{
      {"version", 1},
      {"blocks", std::move(blocks)},
  };
  if (message.created_at.has_value()) {
    out["created_at"] = core::time::format_iso8601_utc(*message.created_at);
  }
  return out.dump();
}

[[nodiscard]] core::Result<std::string>
required_string(const json& object, std::string_view key, std::string_view context) {
  const auto it = object.find(key);
  if (it == object.end() || !it->is_string()) {
    return std::unexpected(message_parse_error(std::string{context}.append(": expected string ").append(key)));
  }
  return it->get<std::string>();
}

[[nodiscard]] core::Result<std::optional<std::string>>
optional_string(const json& object, std::string_view key, std::string_view context) {
  const auto it = object.find(key);
  if (it == object.end()) {
    return std::optional<std::string>{};
  }
  if (!it->is_string()) {
    return std::unexpected(message_parse_error(std::string{context}.append(": expected string ").append(key)));
  }
  return std::optional<std::string>{it->get<std::string>()};
}

[[nodiscard]] core::Result<bool> required_bool(const json& object, std::string_view key, std::string_view context) {
  const auto it = object.find(key);
  if (it == object.end() || !it->is_boolean()) {
    return std::unexpected(message_parse_error(std::string{context}.append(": expected boolean ").append(key)));
  }
  return it->get<bool>();
}

[[nodiscard]] core::Result<core::Content> content_from_json(const json& value, std::size_t index) {
  if (!value.is_object()) {
    return std::unexpected(message_parse_error("content block is not an object").with("index", std::to_string(index)));
  }

  const auto context = std::string{"content["}.append(std::to_string(index)).append("]");
  auto type = required_string(value, "type", context);
  if (!type) {
    return std::unexpected(std::move(type).error());
  }

  if (*type == "text") {
    auto text = required_string(value, "text", context);
    if (!text) {
      return std::unexpected(std::move(text).error());
    }
    return core::Content{core::TextContent{.text = std::move(*text)}};
  }

  if (*type == "thinking") {
    auto thinking = required_string(value, "thinking", context);
    if (!thinking) {
      return std::unexpected(std::move(thinking).error());
    }
    auto signature = optional_string(value, "signature", context);
    if (!signature) {
      return std::unexpected(std::move(signature).error());
    }
    return core::Content{core::ThinkingContent{.thinking = std::move(*thinking), .signature = std::move(*signature)}};
  }

  if (*type == "tool_use") {
    auto id = required_string(value, "id", context);
    if (!id) {
      return std::unexpected(std::move(id).error());
    }
    auto name = required_string(value, "name", context);
    if (!name) {
      return std::unexpected(std::move(name).error());
    }
    auto input_json = required_string(value, "input_json", context);
    if (!input_json) {
      return std::unexpected(std::move(input_json).error());
    }
    return core::Content{
        core::ToolUseContent{.id = std::move(*id), .name = std::move(*name), .input_json = std::move(*input_json)}};
  }

  if (*type == "tool_result") {
    auto tool_use_id = required_string(value, "tool_use_id", context);
    if (!tool_use_id) {
      return std::unexpected(std::move(tool_use_id).error());
    }
    auto output = required_string(value, "output", context);
    if (!output) {
      return std::unexpected(std::move(output).error());
    }
    auto data_json = optional_string(value, "data_json", context);
    if (!data_json) {
      return std::unexpected(std::move(data_json).error());
    }
    auto is_error = required_bool(value, "is_error", context);
    if (!is_error) {
      return std::unexpected(std::move(is_error).error());
    }
    return core::Content{core::ToolResultContent{
        .tool_use_id = std::move(*tool_use_id),
        .output = std::move(*output),
        .data_json = std::move(*data_json),
        .is_error = *is_error,
    }};
  }

  return std::unexpected(message_parse_error("unknown content block type").with("type", std::move(*type)));
}

[[nodiscard]] core::Result<core::Message> message_from_json(const storage::SessionMessageRecord& record) {
  json parsed;
  try {
    parsed = json::parse(record.content_json);
  } catch (const json::parse_error& error) {
    return std::unexpected(message_parse_error("invalid JSON").with("detail", error.what()));
  }

  if (!parsed.is_object()) {
    return std::unexpected(
        message_parse_error("root is not an object").with("sequence", std::to_string(record.sequence)));
  }
  const auto version_it = parsed.find("version");
  if (version_it == parsed.end() || !version_it->is_number_integer() || version_it->get<int>() != 1) {
    return std::unexpected(
        message_parse_error("unsupported message JSON version").with("sequence", std::to_string(record.sequence)));
  }
  const auto blocks_it = parsed.find("blocks");
  if (blocks_it == parsed.end() || !blocks_it->is_array()) {
    return std::unexpected(
        message_parse_error("blocks is not an array").with("sequence", std::to_string(record.sequence)));
  }

  auto blocks = std::vector<core::Content>{};
  blocks.reserve(blocks_it->size());
  for (std::size_t i = 0; i < blocks_it->size(); ++i) {
    auto block = content_from_json((*blocks_it)[i], i);
    if (!block) {
      return std::unexpected(std::move(block).error().with("sequence", std::to_string(record.sequence)));
    }
    blocks.push_back(std::move(*block));
  }

  auto message = core::Message{
      .role = record.role,
      .blocks = std::move(blocks),
      .created_at = std::nullopt,
  };

  if (!record.created_at.empty()) {
    auto created = core::time::parse_iso8601_utc(record.created_at);
    if (!created) {
      return std::unexpected(std::move(created).error().with("sequence", std::to_string(record.sequence)));
    }
    message.created_at = *created;
  }

  return message;
}

[[nodiscard]] storage::SessionKey key_from(SessionId session_id, AgentKey agent_key) {
  return storage::SessionKey{.session_id = std::move(session_id.value), .agent_key = std::move(agent_key.value)};
}

}  // namespace

Store::Store(storage::SessionRepository& repository) noexcept : repository_{&repository} {}

async::Awaitable<core::Result<void>> Store::append(SessionId session_id, AgentKey agent_key, core::Message message) {
  if (auto valid = validate_key(session_id.value, agent_key.value); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }

  auto appended = co_await repository_->append_message(storage::AppendSessionMessageRequest{
      .session_id = std::move(session_id.value),
      .agent_key = std::move(agent_key.value),
      .role = message.role,
      .content_json = message_to_json(message),
  });
  if (!appended) {
    co_return std::unexpected(std::move(appended).error());
  }
  co_return core::Result<void>{};
}

async::Awaitable<core::Result<std::vector<core::Message>>> Store::load(SessionId session_id, AgentKey agent_key) {
  if (auto valid = validate_key(session_id.value, agent_key.value); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }

  auto rows = co_await repository_->load_messages(key_from(std::move(session_id), std::move(agent_key)));
  if (!rows) {
    co_return std::unexpected(std::move(rows).error());
  }

  auto out = std::vector<core::Message>{};
  out.reserve(rows->size());
  for (const auto& row : *rows) {
    auto message = message_from_json(row);
    if (!message) {
      co_return std::unexpected(std::move(message).error());
    }
    out.push_back(std::move(*message));
  }
  co_return out;
}

async::Awaitable<core::Result<void>>
Store::record_skill_activation(SessionId session_id, AgentKey agent_key, SkillActivationUpdate update) {
  if (auto valid = validate_key(session_id.value, agent_key.value); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }
  if (auto valid = validate_skill_activation_update(update); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }

  auto recorded = co_await repository_->upsert_skill_activation(storage::UpsertSessionSkillActivationRequest{
      .session_id = std::move(session_id.value),
      .agent_key = std::move(agent_key.value),
      .skill_name = std::move(update.name),
      .active = update.active,
  });
  if (!recorded) {
    co_return std::unexpected(std::move(recorded).error());
  }
  co_return core::Result<void>{};
}

async::Awaitable<core::Result<std::vector<SkillActivationRecord>>> Store::load_skill_activations(SessionId session_id,
                                                                                                 AgentKey agent_key) {
  if (auto valid = validate_key(session_id.value, agent_key.value); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }

  auto rows = co_await repository_->load_skill_activations(key_from(std::move(session_id), std::move(agent_key)));
  if (!rows) {
    co_return std::unexpected(std::move(rows).error());
  }

  auto out = std::vector<SkillActivationRecord>{};
  out.reserve(rows->size());
  for (auto& row : *rows) {
    out.push_back(SkillActivationRecord{
        .name = std::move(row.skill_name),
        .active = row.active,
        .created_at = std::move(row.created_at),
        .updated_at = std::move(row.updated_at),
    });
  }
  co_return out;
}

async::Awaitable<core::Result<std::vector<SessionSummary>>> Store::list(ListSessionsOptions options) {
  if (options.agent_key.value.empty()) {
    co_return std::unexpected(invalid_field("agent_key"));
  }

  auto rows = co_await repository_->list_sessions(storage::ListSessionsOptions{
      .agent_key = std::move(options.agent_key.value),
      .limit = options.limit,
  });
  if (!rows) {
    co_return std::unexpected(std::move(rows).error());
  }

  auto out = std::vector<SessionSummary>{};
  out.reserve(rows->size());
  for (auto& row : *rows) {
    auto count = checked_size(row.message_count, "message_count");
    if (!count) {
      co_return std::unexpected(std::move(count).error());
    }
    out.push_back(SessionSummary{
        .session_id = SessionId{.value = std::move(row.session_id)},
        .agent_key = AgentKey{.value = std::move(row.agent_key)},
        .message_count = *count,
        .created_at = std::move(row.created_at),
        .updated_at = std::move(row.updated_at),
    });
  }
  co_return out;
}

}  // namespace orangutan::memory::session
