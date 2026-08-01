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
  line_open_ = true;
  ++text_deltas_;
}

void StreamingPromptSink::on_thinking_delta(std::string_view delta) {
  if (!options_.render_thinking) {
    return;
  }
  stream() << delta << std::flush;
  line_open_ = true;
  ++thinking_deltas_;
}

void StreamingPromptSink::on_tool_start(std::string_view /*id*/, std::string_view name) {
  // Preserve a streamed answer/thinking line across the tool iteration: close
  // an open line before the marker instead of gluing `[tool: ...]` to it.
  if (line_open_) {
    stream() << '\n' << std::flush;
    line_open_ = false;
  }
  stream() << "[tool: " << name << "]\n" << std::flush;
  ++tool_starts_;
}

void StreamingPromptSink::on_done(core::StopReason /*stop_reason*/) {
  // Terminate the streamed answer/thinking line. Tool markers self-terminate,
  // and a marker that already closed a previously open line leaves nothing
  // dangling either; only an actually open line needs the newline.
  if (line_open_) {
    stream() << '\n' << std::flush;
    line_open_ = false;
  }
}

}  // namespace orangutan::cli
