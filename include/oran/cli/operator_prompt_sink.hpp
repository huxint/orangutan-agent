// include/oran/cli/operator_prompt_sink.hpp — terminal approval prompt sink.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/hook/decision.hpp>
#include <oran/hook/event.hpp>
#include <oran/hook/payload.hpp>
#include <oran/hook/sink.hpp>

namespace orangutan::cli {

/// Configuration for the CLI-owned `permission_ask_rendered` sink.
///
/// `scripted_answers` exists for tests and future noninteractive drivers. When
/// it is empty, the sink reads one answer from terminal stdin for each approval
/// prompt. Accepted approval answers are `y`, `yes`, `approve`, `approved`, and
/// `proceed`; accepted denial answers are `n`, `no`, `deny`, `denied`, `reject`,
/// and the empty string.
struct OperatorPromptSinkOptions {
  std::string sink_id{"cli-operator-prompt"};
  std::string operator_identity{"terminal"};
  std::vector<std::string> scripted_answers{};
  bool quiet{false};
};

/// Hook sink that renders permission approval prompts in the terminal.
///
/// The sink only makes decisions for `hook::Event::permission_ask_rendered`.
/// Other blocking events proceed unchanged, so callers can bind the same
/// concrete sink narrowly without needing adapter glue.
class OperatorPromptSink final : public hook::Sink {
public:
  explicit OperatorPromptSink(OperatorPromptSinkOptions options = {});

  [[nodiscard]] std::string_view id() const noexcept override;
  [[nodiscard]] async::Awaitable<core::Result<void>> receive(hook::Event event, hook::Payload payload) override;
  [[nodiscard]] async::Awaitable<core::Result<hook::HookDecision>> handle_blocking(hook::Event event,
                                                                                   hook::Payload payload) override;

  [[nodiscard]] std::size_t prompts_rendered() const noexcept {
    return prompts_rendered_;
  }

  [[nodiscard]] std::size_t answers_consumed() const noexcept {
    return next_scripted_answer_;
  }

private:
  OperatorPromptSinkOptions options_;
  std::size_t next_scripted_answer_{0};
  std::size_t prompts_rendered_{0};
};

}  // namespace orangutan::cli
