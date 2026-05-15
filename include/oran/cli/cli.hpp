// include/oran/cli/cli.hpp — early CLI mode selection and shell entry points.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <oran/core/result.hpp>

namespace orangutan::cli {

enum class CliMode : std::uint8_t {
  help,
  repl,
  single_shot,
};

[[nodiscard]] std::string_view to_string_view(CliMode mode) noexcept;

struct CliOptions {
  std::span<const std::string_view> args{};
  std::span<const std::string_view> repl_lines{};
  bool quiet{false};
};

struct CliResult {
  CliMode mode{CliMode::repl};
  std::size_t prompts_processed{0};
  int exit_code{0};
};

[[nodiscard]] core::Result<CliResult> run(CliOptions options = {});

}  // namespace orangutan::cli
