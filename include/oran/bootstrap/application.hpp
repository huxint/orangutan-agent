#pragma once

#include <string>

#include <oran/agent/prompt.hpp>
#include <oran/core/result.hpp>

namespace orangutan::bootstrap {

struct ApplicationOptions {
  std::string config_path;
  std::string workspace{"."};
  std::string state_directory;
  std::string session_id;
  std::string agent_key{"default"};
  std::string prompt;
};

/// Runs one turn and joins its work before releasing services. An empty state
/// directory selects <workspace>/.orangutan; an empty session ID creates one.
[[nodiscard]] core::Result<agent::PromptResult> run_application(ApplicationOptions options);

}  // namespace orangutan::bootstrap
