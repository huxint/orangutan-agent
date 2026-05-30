// include/oran/cli/streaming_prompt_sink.hpp — terminal streaming-delta sink.

#pragma once

#include <cstddef>
#include <iosfwd>
#include <string_view>

#include <oran/core/stop_reason.hpp>
#include <oran/provider/system.hpp>

namespace orangutan::cli {

/// Configuration for the CLI-owned streaming render sink.
///
/// `out` selects the destination stream; `nullptr` renders to `std::cout` (the
/// production terminal). Tests and other non-terminal drivers inject their own
/// `std::ostream`. `render_thinking` controls whether extended-thinking deltas
/// are echoed live alongside the answer text.
struct StreamingPromptSinkOptions {
  std::ostream* out{nullptr};
  bool render_thinking{true};
};

/// `provider::EventSink` that renders streaming deltas to a terminal as they
/// arrive: answer text and (optionally) thinking text are written and flushed
/// per delta so the operator sees character-by-character output, a one-line
/// `[tool: <name>]` marker announces each tool call, and `on_done` terminates
/// the streamed answer line.
///
/// The sink writes only what the provider streams. It reports
/// `rendered_answer_text()` so the prompt runner can suppress the duplicate
/// final-text print when the answer already appeared live; a non-streaming
/// provider that fires no text deltas leaves the runner free to print the
/// assembled text itself.
class StreamingPromptSink final : public provider::EventSink {
public:
  explicit StreamingPromptSink(StreamingPromptSinkOptions options = {});

  void on_text_delta(std::string_view delta) override;
  void on_thinking_delta(std::string_view delta) override;
  void on_tool_start(std::string_view id, std::string_view name) override;
  void on_done(core::StopReason stop_reason) override;

  /// Count of answer-text deltas rendered (drives runner duplicate-suppression).
  [[nodiscard]] std::size_t text_deltas_rendered() const noexcept {
    return text_deltas_;
  }

  /// Count of `[tool: <name>]` markers rendered.
  [[nodiscard]] std::size_t tool_starts_rendered() const noexcept {
    return tool_starts_;
  }

  /// True once any answer text streamed. The runner clears the assembled
  /// `PromptRunResult::text` when this holds to avoid printing it twice.
  [[nodiscard]] bool rendered_answer_text() const noexcept {
    return text_deltas_ > 0;
  }

private:
  [[nodiscard]] std::ostream& stream() const noexcept;

  StreamingPromptSinkOptions options_;
  std::size_t text_deltas_{0};
  std::size_t thinking_deltas_{0};
  std::size_t tool_starts_{0};
};

}  // namespace orangutan::cli
