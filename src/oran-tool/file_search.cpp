// src/oran-tool/file_search.cpp — `file.search` built-in.
//
// Slice 20 shipped literal-substring matching; slice 24 adds the
// `"regex": true` opt-in tracked in `docs/exec-plans/tech-debt-tracker.md`.
// Slice 39 resolves the input path through `tool::Workspace::resolve_list`
// when `DispatchContext::workspace` is supplied so a directory search cannot
// escape the workspace via traversal or a root-side symlink. Nested entries
// continue to skip symlinks wholesale during the recursive walk — a stricter
// form of the same "symlinks may only follow when they stay inside the
// workspace" rule.
// Scans a UTF-8 text file or (recursively) a directory for matches and
// renders one `path:line:text` line per match. The recursive walk skips
// files containing NUL bytes in their first 8 KiB (a ripgrep-style binary
// heuristic) so the agent loop never gets a wall of non-text noise after
// asking the tool to grep a tree. Dotfiles and dot-directories are skipped
// by default; `include_hidden=true` opts in.
//
// Default match mode is literal substring (`pattern` is a no-escape direct
// match). When `regex=true`, the pattern is compiled once per call via
// `permission::InputPattern` (which forward-declares `re2::RE2` so this TU
// stays off `<re2/re2.h>` per rule C6) and each line is tested with
// `RE2::PartialMatch`. Invalid patterns surface as `invalid_argument` with
// the re2 error message attached as `regex_error`, matching the
// rule-side input-pattern compile-error shape.

#include <oran/tool/builtins.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/cancellation_type.hpp>
#include <asio/post.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <nlohmann/json.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/capability.hpp>
#include <oran/core/error.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/permission/input_pattern.hpp>
#include <oran/tool/registry.hpp>
#include <oran/tool/workspace.hpp>

namespace orangutan::tool {

namespace {

constexpr std::string_view kFileSearchSchema =
    R"({"type":"object","properties":{"path":{"type":"string"},"pattern":{"type":"string"},)"
    R"("max_matches":{"type":"integer","minimum":1},"include_hidden":{"type":"boolean"},)"
    R"("regex":{"type":"boolean"}},)"
    R"("required":["path","pattern"],"additionalProperties":false})";

/// Default cap on how many matches a single call returns. Keeps responses
/// LLM-digestible even when the pattern is unusually permissive against the
/// scanned tree.
constexpr std::size_t kDefaultMaxMatches = 100U;

/// Single-file size cap; mirrors `io::ReadTextOptions::max_bytes`. A directory
/// walk silently skips files larger than this; a single-file call surfaces it
/// as `invalid_argument`.
constexpr std::uintmax_t kMaxFileBytes = 16U * 1024U * 1024U;

/// Bytes of file head inspected for the binary-content heuristic.
constexpr std::size_t kBinaryProbeBytes = 8U * 1024U;

struct SearchOptions {
  std::string path;
  std::string pattern;
  std::size_t max_matches{kDefaultMaxMatches};
  bool include_hidden{false};
  bool regex{false};
};

struct Match {
  std::string path;
  std::size_t line_number{0};
  std::string text;
};

struct SearchOutcome {
  std::vector<Match> matches;
  bool truncated{false};
};

/// Per-call line matcher. Owns the compiled regex when the caller opted in
/// (`regex=true`); otherwise it falls back to the literal `pattern` substring.
/// Holding the engaged `InputPattern` here lets `scan_text` stay regex-shape-
/// blind and keeps the cost of "literal mode" identical to the slice-20 path.
struct LineMatcher {
  std::string_view literal;
  std::optional<permission::InputPattern> regex;

  [[nodiscard]] bool matches(std::string_view line) const noexcept {
    return regex ? regex->matches(line) : line.contains(literal);
  }
};

[[nodiscard]] core::Result<SearchOptions> parse_input(std::string_view input_json) {
  nlohmann::json parsed;
  try {
    parsed = nlohmann::json::parse(input_json);
  } catch (const nlohmann::json::parse_error& e) {
    return std::unexpected(
        core::Error::invalid_argument("file.search: input is not valid JSON").with("detail", e.what()));
  } catch (const std::exception& e) {
    return std::unexpected(
        core::Error::invalid_argument("file.search: input is not valid JSON").with("detail", e.what()));
  }

  if (!parsed.is_object()) {
    return std::unexpected(core::Error::invalid_argument("file.search: input must be a JSON object"));
  }
  if (!parsed.contains("path") || !parsed["path"].is_string()) {
    return std::unexpected(core::Error::invalid_argument("file.search: input must include a string `path` field"));
  }
  if (!parsed.contains("pattern") || !parsed["pattern"].is_string()) {
    return std::unexpected(core::Error::invalid_argument("file.search: input must include a string `pattern` field"));
  }

  SearchOptions options;
  options.path = parsed["path"].get<std::string>();
  options.pattern = parsed["pattern"].get<std::string>();

  if (options.pattern.empty()) {
    return std::unexpected(core::Error::invalid_argument("file.search: `pattern` must be non-empty"));
  }

  if (parsed.contains("max_matches")) {
    if (!parsed["max_matches"].is_number_integer() || parsed["max_matches"].is_number_float()) {
      return std::unexpected(core::Error::invalid_argument("file.search: `max_matches` must be a positive integer"));
    }
    const auto raw = parsed["max_matches"].get<std::int64_t>();
    if (raw <= 0) {
      return std::unexpected(core::Error::invalid_argument("file.search: `max_matches` must be a positive integer")
                                 .with("value", std::to_string(raw)));
    }
    options.max_matches = static_cast<std::size_t>(raw);
  }

  if (parsed.contains("include_hidden")) {
    if (!parsed["include_hidden"].is_boolean()) {
      return std::unexpected(core::Error::invalid_argument("file.search: `include_hidden` must be a boolean"));
    }
    options.include_hidden = parsed["include_hidden"].get<bool>();
  }

  if (parsed.contains("regex")) {
    if (!parsed["regex"].is_boolean()) {
      return std::unexpected(core::Error::invalid_argument("file.search: `regex` must be a boolean"));
    }
    options.regex = parsed["regex"].get<bool>();
  }

  return options;
}

[[nodiscard]] bool is_hidden(const std::filesystem::path& p) {
  const auto name = p.filename().string();
  return !name.empty() && name.front() == '.';
}

[[nodiscard]] bool looks_binary(std::string_view contents) {
  const auto probe = contents.substr(0, std::min<std::size_t>(contents.size(), kBinaryProbeBytes));
  return std::ranges::any_of(probe, [](char c) { return c == '\0'; });
}

[[nodiscard]] bool is_cancelled(const asio::cancellation_state& cancellation) noexcept {
  return cancellation.cancelled() != asio::cancellation_type::none;
}

/// Reads a regular file's contents into a string, capped at `kMaxFileBytes`.
/// Returns an io error on open or read failure; an invalid_argument error
/// when the file overshoots the cap (so the agent loop sees a clear "you
/// asked to search something pathologically large" signal in the single-file
/// case). Polls `cancellation` once per 8 KiB read chunk so a pathological
/// multi-GB file aborts promptly when the agent loop is torn down.
[[nodiscard]] core::Result<std::string> read_text_capped(const std::filesystem::path& path,
                                                         const asio::cancellation_state& cancellation) {
  errno = 0;
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    return std::unexpected(core::Error::io("file.search: failed to open file").with("path", path.string()));
  }
  std::string contents;
  std::array<char, 8192> buffer{};
  while (input) {
    if (is_cancelled(cancellation)) {
      return std::unexpected(core::Error::cancelled());
    }
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0) {
      const auto next_size = static_cast<std::uintmax_t>(contents.size()) + static_cast<std::uintmax_t>(count);
      if (next_size > kMaxFileBytes) {
        return std::unexpected(core::Error::invalid_argument("file.search: file exceeds max bytes")
                                   .with("path", path.string())
                                   .with("max_bytes", std::to_string(kMaxFileBytes)));
      }
      contents.append(buffer.data(), static_cast<std::size_t>(count));
    }
  }
  if (input.bad()) {
    return std::unexpected(core::Error::io("file.search: failed while reading file").with("path", path.string()));
  }
  return contents;
}

/// Scans `contents` line-by-line via `matcher` and appends each match to
/// `out`. Stops the moment `out.size()` reaches `budget` (matches are
/// already in the bag). The caller passes `budget = max_matches + 1` so that
/// hitting the budget is a reliable truncation signal: if the final tally
/// exceeds `max_matches`, at least one further match existed.
void scan_text(std::string_view contents,
               const LineMatcher& matcher,
               std::string_view file_path,
               std::vector<Match>& out,
               std::size_t budget) {
  std::size_t line_number = 1;
  std::size_t cursor = 0;
  while (out.size() < budget) {
    const auto next_newline = contents.find('\n', cursor);
    const auto line_end = (next_newline == std::string_view::npos) ? contents.size() : next_newline;
    const auto line = contents.substr(cursor, line_end - cursor);
    if (matcher.matches(line)) {
      out.push_back(Match{
          .path = std::string{file_path},
          .line_number = line_number,
          .text = std::string{line},
      });
    }
    if (next_newline == std::string_view::npos) {
      break;
    }
    cursor = next_newline + 1;
    ++line_number;
  }
}

[[nodiscard]] core::Result<LineMatcher> build_matcher(const SearchOptions& opts) {
  if (!opts.regex) {
    return LineMatcher{.literal = opts.pattern, .regex = std::nullopt};
  }
  auto compiled = permission::InputPattern::compile(opts.pattern);
  if (!compiled) {
    auto err = core::Error::invalid_argument("file.search: invalid regex").with("pattern", opts.pattern);
    for (const auto& [key, value] : compiled.error().context()) {
      err.with(key, value);
    }
    return std::unexpected(std::move(err));
  }
  return LineMatcher{.literal = {}, .regex = std::move(*compiled)};
}

[[nodiscard]] core::Result<SearchOutcome> walk_and_scan(const SearchOptions& opts,
                                                        const asio::cancellation_state& cancellation) {
  auto matcher = build_matcher(opts);
  if (!matcher) {
    return std::unexpected(std::move(matcher).error());
  }

  SearchOutcome outcome;
  const std::size_t budget = opts.max_matches + 1U;

  std::error_code stat_ec;
  const std::filesystem::path root{opts.path};
  if (!std::filesystem::exists(root, stat_ec)) {
    if (stat_ec) {
      return std::unexpected(core::Error::io("file.search: failed to stat path")
                                 .with("path", opts.path)
                                 .with("system_error", stat_ec.message()));
    }
    return std::unexpected(core::Error::not_found("file.search: path does not exist").with("path", opts.path));
  }

  if (std::filesystem::is_regular_file(root, stat_ec)) {
    auto contents = read_text_capped(root, cancellation);
    if (!contents) {
      return std::unexpected(std::move(contents).error());
    }
    // Single-file mode does NOT apply the binary heuristic — the caller named
    // this file explicitly, so we trust their intent. The heuristic only
    // filters during recursive directory walks.
    scan_text(*contents, *matcher, root.string(), outcome.matches, budget);
  } else if (std::filesystem::is_directory(root, stat_ec)) {
    try {
      const auto walk_opts = std::filesystem::directory_options::skip_permission_denied;
      const std::filesystem::recursive_directory_iterator end{};
      auto it = std::filesystem::recursive_directory_iterator{root, walk_opts};
      while (it != end && outcome.matches.size() < budget) {
        // Poll cancellation once per directory entry so a SIGINT mid-walk
        // unwinds the recursion before the next stat / open syscall. The
        // load is a relaxed atomic read — single-digit nanoseconds, lost in
        // the directory-iterator cost.
        if (is_cancelled(cancellation)) {
          return std::unexpected(core::Error::cancelled());
        }
        const auto& entry = *it;
        const auto& entry_path = entry.path();

        std::error_code probe_ec;
        if (!opts.include_hidden && is_hidden(entry_path)) {
          if (entry.is_directory(probe_ec)) {
            it.disable_recursion_pending();
          }
          ++it;
          continue;
        }
        if (entry.is_symlink(probe_ec)) {
          ++it;
          continue;
        }
        if (!entry.is_regular_file(probe_ec)) {
          ++it;
          continue;
        }

        if (auto file_contents = read_text_capped(entry_path, cancellation); file_contents) {
          if (!looks_binary(*file_contents)) {
            scan_text(*file_contents, *matcher, entry_path.string(), outcome.matches, budget);
          }
        } else if (file_contents.error().kind() == core::ErrorKind::cancelled) {
          return std::unexpected(std::move(file_contents).error());
        }  // other unreadable / oversized files are silently skipped during a walk
        ++it;
      }
    } catch (const std::filesystem::filesystem_error& e) {
      return std::unexpected(core::Error::io("file.search: failed to walk directory")
                                 .with("path", opts.path)
                                 .with("system_error", e.code().message()));
    }
  } else {
    return std::unexpected(core::Error::invalid_argument("file.search: path is neither a regular file nor a directory")
                               .with("path", opts.path));
  }

  if (outcome.matches.size() > opts.max_matches) {
    outcome.truncated = true;
    outcome.matches.resize(opts.max_matches);
  }
  return outcome;
}

[[nodiscard]] std::string render(const SearchOutcome& outcome, std::size_t max_matches) {
  if (outcome.matches.empty() && !outcome.truncated) {
    return "no matches";
  }
  std::string text;
  text.reserve(outcome.matches.size() * 80U);
  for (const auto& m : outcome.matches) {
    if (!text.empty()) {
      text.push_back('\n');
    }
    std::format_to(std::back_inserter(text), "{}:{}:{}", m.path, m.line_number, m.text);
  }
  if (outcome.truncated) {
    if (!text.empty()) {
      text.push_back('\n');
    }
    std::format_to(std::back_inserter(text), "(truncated; matches capped at {})", max_matches);
  }
  return text;
}

[[nodiscard]] async::Awaitable<core::Result<Output>> file_search_handler(std::string_view input_json,
                                                                         DispatchContext& ctx) {
  auto opts = parse_input(input_json);
  if (!opts) {
    co_return std::unexpected(std::move(opts).error());
  }

  if (ctx.workspace != nullptr) {
    auto resolved = ctx.workspace->resolve_list(opts->path);
    if (!resolved) {
      co_return std::unexpected(std::move(resolved).error());
    }
    opts->path = std::move(resolved->absolute_path);
  }

  // One executor hop so the blocking filesystem walk runs on the runtime's
  // thread pool rather than the calling strand — matches `io::read_text_file`'s
  // discipline.
  auto cancellation = co_await asio::this_coro::cancellation_state;
  if (is_cancelled(cancellation)) {
    co_return std::unexpected(core::Error::cancelled());
  }
  co_await asio::post(ctx.executor, asio::use_awaitable);
  if (is_cancelled(cancellation)) {
    co_return std::unexpected(core::Error::cancelled());
  }

  auto outcome = walk_and_scan(*opts, cancellation);
  if (!outcome) {
    co_return std::unexpected(std::move(outcome).error());
  }
  co_return Output{.text = render(*outcome, opts->max_matches)};
}

}  // namespace

core::Result<void> register_file_search(Registry& registry) {
  core::ToolDef def{
      .name = std::string{kFileSearchName},
      .description = "Search a UTF-8 text file or (recursively) a directory for matches. "
                     "Input: {\"path\": <string>, \"pattern\": <string>, \"max_matches\"?: uint (default 100), "
                     "\"include_hidden\"?: bool (default false), \"regex\"?: bool (default false)}. "
                     "Default mode treats `pattern` as a literal substring (no-escape). When `regex=true`, "
                     "`pattern` is compiled as a re2 expression and matched against each line via partial match; "
                     "an invalid pattern returns `invalid_argument` with the re2 error attached. "
                     "Returns one `path:line:text` line per match; when the cap is hit a trailing "
                     "`(truncated; matches capped at <N>)` line is appended. Files containing NUL bytes in their "
                     "first 8 KiB are treated as binary and skipped during a directory walk. Returns the literal "
                     "text `no matches` (non-error) when nothing matched.",
      .input_schema_json = std::string{kFileSearchSchema},
      .required_capabilities = {core::Capability::read_file},
  };
  return registry.add(std::move(def), &file_search_handler);
}

}  // namespace orangutan::tool
