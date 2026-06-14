// src/oran-cli/cli.cpp — deterministic CLI mode selection and prompt handoff.

#include <oran/cli/cli.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <expected>
#include <iterator>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

struct ReplInputLine {
  std::string line;
  bool reached_eof{false};
};

enum class ReplLineDisposition : std::uint8_t {
  prompt,
  handled,
  exit,
};

enum class ReplCommand : std::uint8_t {
  none,
  help,
  exit,
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
  std::println(
      "Configured provider routes run prompts through the agent loop; no-route runs only use the local CLI shell.");
}

void print_repl_help() {
  std::println("commands:");
  std::println("  /help  show REPL commands");
  std::println("  /exit  exit the REPL");
  std::println("  /quit  exit the REPL");
}

void print_repl_banner(bool has_runner, bool reads_terminal) {
  std::println("cli mode: repl");
  if (reads_terminal) {
    std::println("REPL shell is ready. Enter an empty line or /exit to exit. Use /help for commands.");
    return;
  }
  if (has_runner) {
    std::println("REPL shell is ready. Use /help for commands.");
    return;
  }
  std::println("REPL shell is ready without a provider route. Configure a route to run the agent loop.");
  std::println("Use /help for commands.");
}

void print_single_shot(std::string_view prompt, bool has_runner) {
  std::println("cli mode: single-shot");
  std::println("prompt: {}", prompt);
  if (!has_runner) {
    std::println("no provider route configured; prompt was not sent to the agent loop.");
  }
}

void print_scripted_prompt(std::string_view prompt, bool has_runner) {
  std::println("> {}", prompt);
  if (!has_runner) {
    std::println("no provider route configured; prompt was not sent to the agent loop.");
  }
}

void print_prompt_result(const PromptRunResult& result) {
  if (!result.text.empty()) {
    std::println("{}", result.text);
  }
}

void print_repl_prompt() {
  std::print("> ");
  std::fflush(stdout);
}

[[nodiscard]] bool is_ascii_space(char ch) noexcept {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

[[nodiscard]] std::string_view trim_ascii_space(std::string_view text) noexcept {
  while (!text.empty() && is_ascii_space(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && is_ascii_space(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

[[nodiscard]] ReplCommand parse_repl_command(std::string_view line) noexcept {
  const auto command = trim_ascii_space(line);
  if (command == "/help") {
    return ReplCommand::help;
  }
  if (command == "/exit" || command == "/quit") {
    return ReplCommand::exit;
  }
  return ReplCommand::none;
}

[[nodiscard]] ReplLineDisposition handle_repl_command(std::string_view line, bool quiet) {
  switch (parse_repl_command(line)) {
    case ReplCommand::help:
      if (!quiet) {
        print_repl_help();
      }
      return ReplLineDisposition::handled;
    case ReplCommand::exit:
      return ReplLineDisposition::exit;
    case ReplCommand::none:
      return ReplLineDisposition::prompt;
  }
  return ReplLineDisposition::prompt;
}

[[nodiscard]] async::Awaitable<Result<ReplInputLine>> read_terminal_line(asio::posix::stream_descriptor& input,
                                                                         asio::streambuf& buffer) {
  auto read_ec = asio::error_code{};
  [[maybe_unused]] const auto bytes =
      co_await asio::async_read_until(input, buffer, '\n', asio::redirect_error(asio::use_awaitable, read_ec));
  if (read_ec && read_ec != asio::error::eof) {
    co_return std::unexpected(Error::io("failed to read REPL input").with("asio_error", read_ec.message()));
  }

  auto data = buffer.data();
  const auto begin = asio::buffers_begin(data);
  const auto end = asio::buffers_end(data);
  const auto newline = std::find(begin, end, '\n');
  const bool reached_eof = read_ec == asio::error::eof && newline == end;
  auto line = std::string{begin, newline};
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  auto consume_count = static_cast<std::size_t>(std::distance(begin, newline));
  if (newline != end) {
    ++consume_count;
  }
  buffer.consume(consume_count);
  co_return ReplInputLine{.line = std::move(line), .reached_eof = reached_eof};
}

[[nodiscard]] async::Awaitable<Result<void>>
dispatch_repl_prompt(PromptRunner* runner, std::string prompt, std::size_t prompt_index, bool quiet, bool echo_prompt) {
  if (!quiet && echo_prompt) {
    print_scripted_prompt(prompt, runner != nullptr);
  }
  if (runner != nullptr) {
    auto prompt_result = co_await runner->run_prompt(PromptRunRequest{
        .prompt = std::move(prompt),
        .mode = CliMode::repl,
        .prompt_index = prompt_index,
    });
    if (!prompt_result) {
      co_return std::unexpected(std::move(prompt_result).error());
    }
    if (!quiet) {
      print_prompt_result(*prompt_result);
    }
  }
  co_return Result<void>{};
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
    print_repl_banner(false, false);
  }
  auto prompts_processed = std::size_t{0};
  for (const auto line : options.repl_lines) {
    if (line.empty()) {
      continue;
    }
    switch (handle_repl_command(line, options.quiet)) {
      case ReplLineDisposition::handled:
        continue;
      case ReplLineDisposition::exit:
        return CliResult{.mode = CliMode::repl, .prompts_processed = prompts_processed, .exit_code = 0};
      case ReplLineDisposition::prompt:
        break;
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

  auto prompts = std::vector<std::string>{};
  prompts.reserve(options.repl_lines.size());
  for (const auto line : options.repl_lines) {
    if (!line.empty()) {
      prompts.emplace_back(line);
    }
  }
  const bool interactive = options.interactive_repl && runner != nullptr && options.repl_lines.empty();
  if (!options.quiet) {
    print_repl_banner(runner != nullptr, interactive);
  }
  auto prompts_processed = std::size_t{0};

  if (interactive) {
    const auto executor = co_await asio::this_coro::executor;
    const int fd = ::dup(STDIN_FILENO);
    if (fd < 0) {
      co_return std::unexpected(Error::io("failed to duplicate stdin").with("errno", std::to_string(errno)));
    }
    asio::posix::stream_descriptor input{executor};
    auto assign_ec = asio::error_code{};
    input.assign(fd, assign_ec);
    if (assign_ec) {
      ::close(fd);
      co_return std::unexpected(Error::io("failed to attach stdin").with("asio_error", assign_ec.message()));
    }
    auto input_buffer = asio::streambuf{};
    while (true) {
      if (!options.quiet) {
        print_repl_prompt();
      }
      auto line = co_await read_terminal_line(input, input_buffer);
      if (!line) {
        co_return std::unexpected(std::move(line).error());
      }
      if (line->line.empty()) {
        break;
      }
      switch (handle_repl_command(line->line, options.quiet)) {
        case ReplLineDisposition::handled:
          if (line->reached_eof) {
            co_return CliResult{.mode = CliMode::repl, .prompts_processed = prompts_processed, .exit_code = 0};
          }
          continue;
        case ReplLineDisposition::exit:
          co_return CliResult{.mode = CliMode::repl, .prompts_processed = prompts_processed, .exit_code = 0};
        case ReplLineDisposition::prompt:
          break;
      }
      auto prompt_result =
          co_await dispatch_repl_prompt(runner, std::move(line->line), prompts_processed, options.quiet, false);
      if (!prompt_result) {
        co_return std::unexpected(std::move(prompt_result).error());
      }
      ++prompts_processed;
      if (line->reached_eof) {
        break;
      }
    }
    co_return CliResult{.mode = CliMode::repl, .prompts_processed = prompts_processed, .exit_code = 0};
  }

  for (auto& prompt : prompts) {
    switch (handle_repl_command(prompt, options.quiet)) {
      case ReplLineDisposition::handled:
        continue;
      case ReplLineDisposition::exit:
        co_return CliResult{.mode = CliMode::repl, .prompts_processed = prompts_processed, .exit_code = 0};
      case ReplLineDisposition::prompt:
        break;
    }
    auto prompt_result =
        co_await dispatch_repl_prompt(runner, std::move(prompt), prompts_processed, options.quiet, true);
    if (!prompt_result) {
      co_return std::unexpected(std::move(prompt_result).error());
    }
    ++prompts_processed;
  }
  co_return CliResult{.mode = CliMode::repl, .prompts_processed = prompts_processed, .exit_code = 0};
}

}  // namespace orangutan::cli
