#pragma once

#include "host.hpp"

namespace orangutan::bootstrap {
class RuntimeAssembly;
}
namespace orangutan::provider {
struct Route;
}

namespace orangutan::telegram_host {
struct Command {
  std::string name;
  bool has_arguments{false};
  bool addressed_here{true};
};

[[nodiscard]] std::optional<Command> parse_command(std::string_view text, std::string_view bot_username);
[[nodiscard]] Json command_menu(std::string_view user);
[[nodiscard]] std::string command_help();

/// Called after host authorization; storage operations run on the worker executor.
[[nodiscard]] async::Awaitable<core::Result<std::string>> session_status(bootstrap::RuntimeAssembly& assembly,
                                                                         const provider::Route& route,
                                                                         core::TurnId session,
                                                                         asio::any_io_executor worker);
}  // namespace orangutan::telegram_host
