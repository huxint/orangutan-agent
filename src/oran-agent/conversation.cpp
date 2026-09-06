#include <oran/agent/conversation.hpp>

#include <algorithm>
#include <utility>
#include <variant>

namespace orangutan::agent {

PreparedConversation prepare_conversation(std::vector<core::Message> history, std::string prompt) {
  const auto first_user = std::ranges::find_if(history, [](const core::Message& message) {
    return message.role == core::Role::user && std::ranges::any_of(message.blocks, [](const core::Content& block) {
             return std::holds_alternative<core::TextContent>(block);
           });
  });
  history.erase(history.begin(), first_user);
  const auto history_size = history.size();
  history.push_back(core::Message::user_text(std::move(prompt)));
  return PreparedConversation{.messages = std::move(history), .history_size = history_size};
}

}  // namespace orangutan::agent
