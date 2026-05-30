// src/oran-cli/streaming_prompt_sink.cpp — terminal streaming-delta sink.

#include <oran/cli/streaming_prompt_sink.hpp>

#include <iostream>
#include <ostream>
#include <string_view>

namespace orangutan::cli {

StreamingPromptSink::StreamingPromptSink(StreamingPromptSinkOptions options) : options_{options} {}

std::ostream& StreamingPromptSink::stream() const noexcept {
  return options_.out != nullptr ? *options_.out : std::cout;
}

void StreamingPromptSink::on_text_delta(std::string_view delta) {
  stream() << delta << std::flush;
  ++text_deltas_;
}

void StreamingPromptSink::on_thinking_delta(std::string_view delta) {
  if (!options_.render_thinking) {
    return;
  }
  stream() << delta << std::flush;
  ++thinking_deltas_;
}

void StreamingPromptSink::on_tool_start(std::string_view /*id*/, std::string_view name) {
  stream() << "[tool: " << name << "]\n" << std::flush;
  ++tool_starts_;
}

void StreamingPromptSink::on_done(core::StopReason /*stop_reason*/) {
  // Terminate the streamed answer/thinking line. Tool markers self-terminate,
  // so a turn that streamed only tool calls leaves no dangling line to close.
  if (text_deltas_ > 0 || thinking_deltas_ > 0) {
    stream() << '\n' << std::flush;
  }
}

}  // namespace orangutan::cli
