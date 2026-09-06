#pragma once

#include <optional>
#include <string>

#include <oran/core/turn_id.hpp>

namespace orangutan::agent {

struct PromptRequest {
  std::string prompt;
  std::optional<core::TurnId> turn_id{};
};

struct PromptResult {
  std::string text;
};

}  // namespace orangutan::agent
