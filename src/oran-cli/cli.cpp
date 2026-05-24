// src/oran-cli/cli.cpp — deterministic CLI mode selection and prompt handoff.

#include <oran/cli/cli.hpp>

#include <expected>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <oran/core/error.hpp>

namespace orangutan::cli {
namespace {

using ::orangutan::core::Error;
using ::orangutan::core::Result;

struct ParsedArgs {
  CliMode mode{CliMode::repl};
  bool has_prompt{false};
  std::string prompt{};
};

[[nodiscard]] Error arg_error(std::string message) {
  return Error::invalid_argument(std::move(message));
}

[[nodiscard]] Result<ParsedArgs> parse_args(std::span<const std::string_view> args) {
  auto parsed = ParsedArgs{};

  for (auto index = std::size_t{0}; index < args.size(); ++index) {
    const auto arg = args[index];
    if (arg == "--") {
      continue;
    }

    if (arg == "--help" || arg == "-h") {
      parsed.mode = CliMode::help;
      continue;
    }
    if (parsed.mode == CliMode::help) {
      continue;
    }

    constexpr auto kPromptPrefix = std::string_view{"--prompt="};
    if (arg.starts_with(kPromptPrefix)) {
      if (parsed.has_prompt) {
        return std::unexpected(arg_error("--prompt may be provided only once"));
      }
      parsed.has_prompt = true;
      parsed.mode = CliMode::single_shot;
      parsed.prompt = std::string{arg.substr(kPromptPrefix.size())};
      if (parsed.prompt.empty()) {
        return std::unexpected(arg_error("--prompt requires a non-empty value"));
      }
      continue;
    }

    if (arg == "--prompt") {
      if (parsed.has_prompt) {
        return std::unexpected(arg_error("--prompt may be provided only once"));
      }
      if (index + 1 >= args.size()) {
        return std::unexpected(arg_error("--prompt requires a value"));
      }
      parsed.has_prompt = true;
      parsed.mode = CliMode::single_shot;
      parsed.prompt = std::string{args[++index]};
      if (parsed.prompt.empty()) {
        return std::unexpected(arg_error("--prompt requires a non-empty value"));
      }
      continue;
    }

    return std::unexpected(arg_error("unknown CLI argument").with("arg", std::string{arg}));
  }

  return parsed;
}

void print_usage() {
  std::println("usage: orangutan [--config <path>] [--prompt <text>] [--help]");
  std::println();
  std::println("The current CLI slice accepts prompts but does not run the agent loop yet.");
}

void print_repl_banner(bool has_runner) {
  std::println("cli mode: repl");
  if (has_runner) {
    std::println("REPL shell is ready.");
    return;
  }
  std::println("REPL shell is ready, but the agent loop is not implemented yet.");
}

void print_single_shot(std::string_view prompt, bool has_runner) {
  std::println("cli mode: single-shot");
  std::println("prompt: {}", prompt);
  if (!has_runner) {
    std::println("agent loop is not implemented yet.");
  }
}

void print_scripted_prompt(std::string_view prompt, bool has_runner) {
  std::println("> {}", prompt);
  if (!has_runner) {
    std::println("agent loop is not implemented yet.");
  }
}

void print_prompt_result(const PromptRunResult& result) {
  if (!result.text.empty()) {
    std::println("{}", result.text);
  }
}

}  // namespace

std::string_view to_string_view(CliMode mode) noexcept {
  switch (mode) {
    case CliMode::help:
      return "help";
    case CliMode::repl:
      return "repl";
    case CliMode::single_shot:
      return "single-shot";
  }
  return "unknown";
}

core::Result<CliResult> run(CliOptions options) {
  auto parsed = parse_args(options.args);
  if (!parsed) {
    return std::unexpected(std::move(parsed.error()));
  }

  if (parsed->mode == CliMode::help) {
    if (!options.quiet) {
      print_usage();
    }
    return CliResult{.mode = CliMode::help, .prompts_processed = 0, .exit_code = 0};
  }

  if (parsed->mode == CliMode::single_shot) {
    if (!options.quiet) {
      print_single_shot(parsed->prompt, false);
    }
    return CliResult{.mode = CliMode::single_shot, .prompts_processed = 1, .exit_code = 0};
  }

  if (!options.quiet) {
    print_repl_banner(false);
  }
  auto prompts_processed = std::size_t{0};
  for (const auto line : options.repl_lines) {
    if (line.empty()) {
      continue;
    }
    ++prompts_processed;
    if (!options.quiet) {
      print_scripted_prompt(line, false);
    }
  }
  return CliResult{.mode = CliMode::repl, .prompts_processed = prompts_processed, .exit_code = 0};
}

async::Awaitable<core::Result<CliResult>> run_async(CliOptions options, PromptRunner* runner) {
  auto parsed = parse_args(options.args);
  if (!parsed) {
    co_return std::unexpected(std::move(parsed.error()));
  }

  if (parsed->mode == CliMode::help) {
    if (!options.quiet) {
      print_usage();
    }
    co_return CliResult{.mode = CliMode::help, .prompts_processed = 0, .exit_code = 0};
  }

  if (parsed->mode == CliMode::single_shot) {
    if (!options.quiet) {
      print_single_shot(parsed->prompt, runner != nullptr);
    }
    if (runner != nullptr) {
      auto prompt_result = co_await runner->run_prompt(PromptRunRequest{
          .prompt = std::move(parsed->prompt),
          .mode = CliMode::single_shot,
          .prompt_index = 0,
      });
      if (!prompt_result) {
        co_return std::unexpected(std::move(prompt_result).error());
      }
      if (!options.quiet) {
        print_prompt_result(*prompt_result);
      }
    }
    co_return CliResult{.mode = CliMode::single_shot, .prompts_processed = 1, .exit_code = 0};
  }

  if (!options.quiet) {
    print_repl_banner(runner != nullptr);
  }
  auto prompts = std::vector<std::string>{};
  prompts.reserve(options.repl_lines.size());
  for (const auto line : options.repl_lines) {
    if (!line.empty()) {
      prompts.emplace_back(line);
    }
  }
  auto prompts_processed = std::size_t{0};
  for (const auto& prompt : prompts) {
    if (!options.quiet) {
      print_scripted_prompt(prompt, runner != nullptr);
    }
    if (runner != nullptr) {
      auto prompt_result = co_await runner->run_prompt(PromptRunRequest{
          .prompt = prompt,
          .mode = CliMode::repl,
          .prompt_index = prompts_processed,
      });
      if (!prompt_result) {
        co_return std::unexpected(std::move(prompt_result).error());
      }
      if (!options.quiet) {
        print_prompt_result(*prompt_result);
      }
    }
    ++prompts_processed;
  }
  co_return CliResult{.mode = CliMode::repl, .prompts_processed = prompts_processed, .exit_code = 0};
}

}  // namespace orangutan::cli
