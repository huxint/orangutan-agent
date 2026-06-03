// include/oran/cli/cli.hpp — CLI mode selection and prompt-runner handoff entry points.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>

namespace orangutan::cli {

enum class CliMode : std::uint8_t {
  help,
  repl,
  single_shot,
};

/// User-facing CLI spelling. `single_shot` renders as `single-shot`, so this
/// cannot use the identifier-style reflection helper directly.
[[nodiscard]] std::string_view to_string_view(CliMode mode) noexcept;

struct CliOptions {
  std::span<const std::string_view> args{};
  // Scripted REPL prompts for tests and noninteractive drivers; empty entries are ignored.
  std::span<const std::string_view> repl_lines{};
  // Enables terminal stdin reads in run_async when a PromptRunner is supplied
  // and repl_lines is empty.
  bool interactive_repl{false};
  bool quiet{false};
};

struct CliResult {
  CliMode mode{CliMode::repl};
  std::size_t prompts_processed{0};
  int exit_code{0};
};

struct PromptRunRequest {
  std::string prompt;
  CliMode mode{CliMode::single_shot};
  std::size_t prompt_index{0};
};

struct PromptRunResult {
  std::string text;
};

class PromptRunner {
public:
  PromptRunner() = default;
  virtual ~PromptRunner() = default;

  PromptRunner(const PromptRunner&) = delete;
  PromptRunner& operator=(const PromptRunner&) = delete;
  PromptRunner(PromptRunner&&) = delete;
  PromptRunner& operator=(PromptRunner&&) = delete;

  [[nodiscard]] virtual async::Awaitable<core::Result<PromptRunResult>> run_prompt(PromptRunRequest request) = 0;
};

[[nodiscard]] core::Result<CliResult> run(CliOptions options = {});

[[nodiscard]] async::Awaitable<core::Result<CliResult>> run_async(CliOptions options = {},
                                                                  PromptRunner* runner = nullptr);

}  // namespace orangutan::cli
