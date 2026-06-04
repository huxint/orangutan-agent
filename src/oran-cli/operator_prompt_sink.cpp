// src/oran-cli/operator_prompt_sink.cpp — terminal approval prompt sink.

#include <oran/cli/operator_prompt_sink.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <expected>
#include <iterator>
#include <optional>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <asio/buffer.hpp>
#include <asio/buffers_iterator.hpp>
#include <asio/posix/stream_descriptor.hpp>
#include <asio/read_until.hpp>
#include <asio/redirect_error.hpp>
#include <asio/streambuf.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <unistd.h>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>

namespace orangutan::cli {
namespace {

[[nodiscard]] bool ascii_space(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

[[nodiscard]] std::string normalize_answer(std::string_view answer) {
  const auto first = std::ranges::find_if_not(answer, ascii_space);
  if (first == answer.end()) {
    return {};
  }
  const auto reversed_tail = std::ranges::find_if_not(answer | std::views::reverse, ascii_space);
  const auto last = reversed_tail.base();

  const auto trimmed = std::string_view{first, static_cast<std::size_t>(last - first)};
  auto normalized = std::string{};
  normalized.reserve(trimmed.size());
  std::ranges::transform(trimmed, std::back_inserter(normalized), [](char c) -> char {
    if (c >= 'A' && c <= 'Z') {
      return static_cast<char>(c - 'A' + 'a');
    }
    return c;
  });
  return normalized;
}

[[nodiscard]] bool is_approval_answer(std::string_view answer) noexcept {
  return answer == "y" || answer == "yes" || answer == "approve" || answer == "approved" || answer == "proceed";
}

[[nodiscard]] bool is_denial_answer(std::string_view answer) noexcept {
  return answer.empty() || answer == "n" || answer == "no" || answer == "deny" || answer == "denied" ||
         answer == "reject";
}

[[nodiscard]] std::string operator_identity(const OperatorPromptSinkOptions& options,
                                            const hook::PermissionAskRenderedPayload& payload) {
  if (!payload.who.identity.empty()) {
    return payload.who.identity;
  }
  return options.operator_identity;
}

[[nodiscard]] hook::HookDecision approve_decision(std::string identity) {
  auto decision = hook::HookDecision{};
  decision.reason = "operator_approved:";
  decision.reason.append(identity);
  return decision;
}

[[nodiscard]] hook::HookDecision deny_decision(std::string identity) {
  auto decision = hook::HookDecision{};
  decision.kind = hook::HookDecisionKind::veto;
  decision.reason = "operator_denied:";
  decision.reason.append(identity);
  return decision;
}

void render_prompt(const hook::PermissionAskRenderedPayload& payload) {
  std::println();
  std::println("permission approval required");
  std::println("tool: {}", payload.tool_name);
  if (!payload.who.scope_key.empty()) {
    std::println("scope: {}", payload.who.scope_key);
  }
  if (!payload.who.agent_key.empty()) {
    std::println("agent: {}", payload.who.agent_key);
  }
  if (!payload.who.identity.empty()) {
    std::println("identity: {}", payload.who.identity);
  }
  if (!payload.decision_reason.empty()) {
    std::println("reason: {}", payload.decision_reason);
  }
  std::println("requested_at: {}", core::time::format_iso8601_utc(payload.requested_at));
  std::println("approval: replay_max={} ttl={}s", payload.replay_max, payload.approval_ttl.count());
  std::println("input: {}", payload.input_json);
}

[[nodiscard]] async::Awaitable<core::Result<std::string>> read_terminal_line() {
  const auto executor = co_await asio::this_coro::executor;
  const int fd = ::dup(STDIN_FILENO);
  if (fd < 0) {
    co_return std::unexpected(core::Error::io("failed to duplicate stdin").with("errno", std::to_string(errno)));
  }

  asio::posix::stream_descriptor input{executor};
  auto assign_ec = asio::error_code{};
  input.assign(fd, assign_ec);
  if (assign_ec) {
    ::close(fd);
    co_return std::unexpected(core::Error::io("failed to attach stdin").with("asio_error", assign_ec.message()));
  }

  auto buffer = asio::streambuf{};
  auto read_ec = asio::error_code{};
  [[maybe_unused]] const auto bytes =
      co_await asio::async_read_until(input, buffer, '\n', asio::redirect_error(asio::use_awaitable, read_ec));
  if (read_ec && read_ec != asio::error::eof) {
    co_return std::unexpected(core::Error::io("failed to read approval answer").with("asio_error", read_ec.message()));
  }

  auto data = buffer.data();
  auto line = std::string{asio::buffers_begin(data), asio::buffers_end(data)};
  if (const auto newline = line.find('\n'); newline != std::string::npos) {
    line.resize(newline);
  }
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  co_return line;
}

[[nodiscard]] core::Error invalid_payload_error(hook::Event event) {
  return core::Error::invalid_argument("operator prompt requires PermissionAskRenderedPayload")
      .with("event", std::string{core::enum_name(event)});
}

[[nodiscard]] core::Error invalid_answer_error(std::string answer) {
  return core::Error::invalid_argument("operator approval answer must be yes or no").with("answer", std::move(answer));
}

}  // namespace

OperatorPromptSink::OperatorPromptSink(OperatorPromptSinkOptions options) : options_(std::move(options)) {}

std::string_view OperatorPromptSink::id() const noexcept {
  return options_.sink_id;
}

async::Awaitable<core::Result<void>> OperatorPromptSink::receive(hook::Event /*event*/, hook::PayloadPtr /*payload*/) {
  co_return core::Result<void>{};
}

async::Awaitable<core::Result<hook::HookDecision>> OperatorPromptSink::handle_blocking(hook::Event event,
                                                                                       hook::PayloadPtr payload) {
  if (event != hook::Event::permission_ask_rendered) {
    co_return hook::HookDecision{};
  }

  const auto* ask = std::get_if<hook::PermissionAskRenderedPayload>(payload.get());
  if (ask == nullptr) {
    co_return std::unexpected(invalid_payload_error(event));
  }

  ++prompts_rendered_;
  const auto identity = operator_identity(options_, *ask);
  if (!options_.quiet) {
    render_prompt(*ask);
  }

  while (true) {
    auto answer = std::string{};
    const bool scripted = next_scripted_answer_ < options_.scripted_answers.size();
    if (scripted) {
      answer = options_.scripted_answers[next_scripted_answer_++];
    } else {
      if (!options_.quiet) {
        std::print("Approve this tool call? [y/N]: ");
        std::fflush(stdout);
      }
      auto line = co_await read_terminal_line();
      if (!line) {
        co_return std::unexpected(std::move(line).error());
      }
      answer = std::move(*line);
    }

    const auto normalized = normalize_answer(answer);
    if (is_approval_answer(normalized)) {
      co_return approve_decision(identity);
    }
    if (is_denial_answer(normalized)) {
      co_return deny_decision(identity);
    }

    if (scripted) {
      co_return std::unexpected(invalid_answer_error(std::move(answer)));
    }
    if (!options_.quiet) {
      std::println("Please answer y or n.");
    }
  }
}

}  // namespace orangutan::cli
