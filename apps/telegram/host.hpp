#pragma once

#include <asio/any_io_executor.hpp>
#include <functional>
#include <nlohmann/json.hpp>
#include <oran/channel/dispatcher.hpp>
#include <oran/core/turn_id.hpp>
#include <oran/io/private_directory.hpp>

namespace orangutan::telegram_host {
using Json = nlohmann::json;
using Api = std::function<async::Awaitable<core::Result<channel::Response>>(channel::Request)>;

struct Options {
  std::string user{};
  bool probe{false};
  bool once{false};
};

/// A pending journal is never replayed automatically, including after a crash.
[[nodiscard]] core::Result<Json>
load_state(const io::PrivateDirectory& directory, std::string_view user, std::string_view workspace);
/// Explicit operator reconciliation: archive pending data, then skip exactly this update.
[[nodiscard]] core::Result<void> acknowledge_pending(const io::PrivateDirectory& directory, std::int64_t update);
[[nodiscard]] bool admits(const channel::Message& message, std::string_view user);
[[nodiscard]] core::Result<Json> api_result(const channel::Response& response);

/// Injected effects are called only after explicit permission/hook decisions.
/// All calls run on one strand; the caller retains services until completion.
[[nodiscard]] async::Awaitable<core::Result<void>> run(Options options,
                                                       Api api,
                                                       channel::Transport outbound,
                                                       channel::RunTurn turn,
                                                       hook::Bus& hooks,
                                                       io::PrivateDirectory& directory,
                                                       Json& state,
                                                       asio::any_io_executor worker);
}  // namespace orangutan::telegram_host
