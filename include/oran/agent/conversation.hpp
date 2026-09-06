#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <oran/core/message.hpp>

namespace orangutan::agent {

struct PreparedConversation {
  std::vector<core::Message> messages;
  std::size_t history_size{};
};

/// Retain the complete exchanges in a history suffix and append this prompt.
/// history_size marks where successful turn persistence must begin.
[[nodiscard]] PreparedConversation prepare_conversation(std::vector<core::Message> history, std::string prompt);

}  // namespace orangutan::agent
