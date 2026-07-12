// src/oran-tool/file_search.cpp — `FileSearch` built-in.
//
// Slice 20 shipped literal-substring matching; slice 24 adds the
// `"regex": true` opt-in tracked in `docs/exec-plans/tech-debt-tracker.md`.
// Slice 63 (2026-05-24) closes spec 0014's "structured `data_json` migration
// for `FileSearch`" item: successful calls keep the existing
// `path:line:text` text rendering for current callers AND fill
// `Output::data_json` with a serialized `{kind:"file_search", path, pattern,
// regex, matches[], match_count, truncated, truncation_reason, files_scanned,
// bytes_read}` payload. `Output::usage` is filled with `bytes_read`
// (cumulative file bytes scanned across the walk), `files_touched`
// (count of non-binary files actually run through the matcher),
// `match_count` (post-truncation match count surfaced to the caller),
// and `truncated` (true when either the match-count cap or the byte cap
// fired).
// Slice 39 resolves the input path through `tool::Workspace::resolve_list`
// when `DispatchContext::workspace` is supplied so a directory search cannot
// escape the workspace via traversal or a root-side symlink. Nested entries
// continue to skip symlinks wholesale during the recursive walk — a stricter
// form of the same "symlinks may only follow when they stay inside the
// workspace" rule. Slice 47 closes spec 0011 v1.1's "Output cap on
// `FileSearch`" item: an optional `max_output_bytes` field (default 1 MiB)
// caps the *rendered* `path:line:text` payload so a handful of very long
// lines cannot flood the prompt even when `max_matches` is generous. When
// the byte cap fires first, the trailing summary line spells out
// `(truncated; output capped at <N> bytes)`; the legacy `(truncated; matches
// capped at <N>)` message still wins when the match-count cap dominates.
// Scans a UTF-8 text file or (recursively) a directory for matches and
// renders one `path:line:text` line per match. The recursive walk skips
// files containing NUL bytes in their first 8 KiB (a ripgrep-style binary
// heuristic) so the agent loop never gets a wall of non-text noise after
// asking the tool to grep a tree. Dotfiles and dot-directories are skipped
// by default; `include_hidden=true` opts in.
//
// Default match mode is literal substring (`pattern` is a no-escape direct
// match). When `regex=true`, the pattern is compiled through
// `permission::InputPattern` (which forward-declares `re2::RE2` so this TU
// stays off `<re2/re2.h>` per rule C6) and each line is tested with
// `RE2::PartialMatch`. Slice 51 keeps compiled patterns in a bounded
// process-local `core::BoundedCache` (64 entries / 64 KiB / 10-minute TTL)
// keyed by pattern + line-match mode, so repeated searches with the same
// regex avoid paying the compile cost again. Invalid patterns surface as
// `invalid_argument` with the re2 error message attached as `regex_error`,
// matching the rule-side input-pattern compile-error shape.
//
// Slice 48 takes the first step on spec 0011 v1.1's "`FileSearch` ignore
// predicate" item by adding a *built-in* skip list: the recursive directory
// walk no longer descends into `build`, `node_modules`, `.git`, `.xmake`, or
// `.orangutan` regardless of the `include_hidden` flag. Slice 49 completes
// the source-controlled ignore-file half for recursive walks, and slice 266
// moves those recursive-walk decisions into `WorkspaceWalkFilter` so
// `DirectoryList` shares the same behavior. The shared implementation covers
// the common Git-style subset agents need most: comments/blanks, escaped
// leading `#` / `!` literals, `!` negation, trailing `/` directory-only rules,
// anchored or relative slash patterns, basename patterns, and fnmatch-style
// `*` / `?` / `[]` globs. Explicit single-file searches still honour the named
// file directly.

#include <oran/tool/builtins.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include <asio/cancellation_type.hpp>
#include <asio/post.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <nlohmann/json.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/bounded_cache.hpp>
#include <oran/core/capability.hpp>
#include <oran/core/error.hpp>
#include <oran/core/time.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/io/directory_authority.hpp>
#include <oran/io/file.hpp>
#include <oran/permission/input_pattern.hpp>
#include <oran/tool/registry.hpp>
#include <oran/tool/workspace.hpp>

#include "_impl/parse_input.hpp"

namespace orangutan::tool {

namespace {

constexpr std::string_view kFileSearchSchema =
    R"({"type":"object","properties":{"path":{"type":"string"},"pattern":{"type":"string"},)"
    R"("max_matches":{"type":"integer","minimum":1},"include_hidden":{"type":"boolean"},)"
    R"("regex":{"type":"boolean"},"max_output_bytes":{"type":"integer","minimum":1},)"
    R"("respect_ignore":{"type":"boolean"},"allow_outside_workspace":{"type":"boolean"}},)"
    R"("required":["path","pattern"],"additionalProperties":false})";

/// Default cap on how many matches a single call returns. Keeps responses
/// LLM-digestible even when the pattern is unusually permissive against the
/// scanned tree.
constexpr std::size_t kDefaultMaxMatches = 100U;

/// Default rendered-output byte cap. Closes spec 0011 v1.1's "Output cap on
/// `FileSearch`" item: even with a modest `max_matches`, a handful of very
/// long lines can flood the prompt. The cap is on *rendered* bytes — the
/// `path:line:text` payload the agent sees — not on the file body. 1 MiB is
/// large enough that no current single-call test trips it; agents that want
/// to be stricter can lower it; agents that want to harvest a very large
/// payload can raise it.
constexpr std::size_t kDefaultMaxOutputBytes = 1U * 1024U * 1024U;

/// Single-file size cap; mirrors `io::ReadTextOptions::max_bytes`. A directory
/// walk silently skips files larger than this; a single-file call surfaces it
/// as `invalid_argument`.
constexpr std::uintmax_t kMaxFileBytes = 16U * 1024U * 1024U;

/// Bytes of file head inspected for the binary-content heuristic.
constexpr std::size_t kBinaryProbeBytes = 8U * 1024U;

constexpr std::size_t kRegexCacheMaxEntries = 64U;
constexpr std::size_t kRegexCacheMaxBytes = 64U * 1024U;

struct RegexCacheKey {
  std::string pattern;
  bool partial_line_match{true};

  friend bool operator==(const RegexCacheKey&, const RegexCacheKey&) = default;
};

struct RegexCacheKeyHash {
  [[nodiscard]] std::size_t operator()(const RegexCacheKey& key) const noexcept {
    const auto h1 = std::hash<std::string>{}(key.pattern);
    const auto h2 = std::hash<bool>{}(key.partial_line_match);
    return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6U) + (h1 >> 2U));
  }
};

struct RegexPatternByteCost {
  [[nodiscard]] std::size_t operator()(const std::shared_ptr<const permission::InputPattern>& pattern) const noexcept {
    return pattern == nullptr ? 0 : sizeof(permission::InputPattern) + pattern->pattern().size();
  }
};

using RegexPatternCache = core::BoundedCache<RegexCacheKey,
                                             std::shared_ptr<const permission::InputPattern>,
                                             RegexPatternByteCost,
                                             RegexCacheKeyHash>;

[[nodiscard]] RegexPatternCache& regex_pattern_cache() {
  static auto cache = RegexPatternCache{
      RegexPatternCache::Options{
          .max_entries = kRegexCacheMaxEntries,
          .max_bytes = kRegexCacheMaxBytes,
          .ttl = std::chrono::minutes{10},
      },
      RegexPatternByteCost{},
  };
  return cache;
}

[[nodiscard]] std::mutex& regex_pattern_cache_mutex() {
  static std::mutex mutex;
  return mutex;
}

struct SearchOptions {
  std::string path;
  std::string pattern;
  std::size_t max_matches{kDefaultMaxMatches};
  std::size_t max_output_bytes{kDefaultMaxOutputBytes};
  bool include_hidden{false};
  bool regex{false};
  bool respect_ignore{true};
  const Workspace* workspace{nullptr};
};

struct Match {
  std::string path;
  std::size_t line_number{0};
  std::string text;
};

/// Why scanning stopped short. The render layer turns this into a trailing
/// `(truncated; ...)` line so the agent can tell *what* limit fired.
enum class TruncReason : std::uint8_t {
  none,
  matches,
  bytes,
};

struct SearchOutcome {
  std::vector<Match> matches;
  TruncReason truncated{TruncReason::none};
  std::uintmax_t bytes_read{0};
  std::uint32_t files_scanned{0};
};

/// The resolved search target, opened through pinned descriptors. Exactly one
/// of `file` / `directory` is populated. `base_absolute` is the absolute
/// spelling used to reconstruct per-entry display paths and to root the
/// ignore-file filter; directory descent never reopens it.
struct SearchRoot {
  std::optional<io::ReadOnlyFile> file;
  std::optional<io::DirectoryAuthority> directory;
  std::string base_absolute;
};

/// Per-call line matcher. Owns the compiled regex when the caller opted in
/// (`regex=true`); otherwise it falls back to the literal `pattern` substring.
/// Holding a shared pointer into the bounded regex cache lets `scan_text` stay
/// regex-shape-blind and keeps the cost of "literal mode" identical to the
/// slice-20 path.
struct LineMatcher {
  std::string_view literal;
  std::shared_ptr<const permission::InputPattern> regex;

  [[nodiscard]] bool matches(std::string_view line) const noexcept {
    return regex ? regex->matches(line) : line.contains(literal);
  }
};

[[nodiscard]] core::Result<SearchOptions> parse_input(std::string_view input_json) {
  auto parsed = detail::parse_input_object(input_json, kFileSearchName);
  if (!parsed) {
    return std::unexpected(std::move(parsed).error());
  }

  auto path_field = detail::require_string_field(*parsed, kFileSearchName, "path");
  if (!path_field) {
    return std::unexpected(std::move(path_field).error());
  }
  auto pattern_field = detail::require_string_field(*parsed, kFileSearchName, "pattern");
  if (!pattern_field) {
    return std::unexpected(std::move(pattern_field).error());
  }

  SearchOptions options;
  options.path = *std::move(path_field);
  options.pattern = *std::move(pattern_field);

  if (options.pattern.empty()) {
    return std::unexpected(core::Error::invalid_argument("FileSearch: `pattern` must be non-empty"));
  }

  if (parsed->contains("max_matches")) {
    if (!(*parsed)["max_matches"].is_number_integer() || (*parsed)["max_matches"].is_number_float()) {
      return std::unexpected(core::Error::invalid_argument("FileSearch: `max_matches` must be a positive integer"));
    }
    const auto raw = (*parsed)["max_matches"].get<std::int64_t>();
    if (raw <= 0) {
      return std::unexpected(core::Error::invalid_argument("FileSearch: `max_matches` must be a positive integer")
                                 .with("value", std::to_string(raw)));
    }
    options.max_matches = static_cast<std::size_t>(raw);
  }

  if (parsed->contains("include_hidden")) {
    if (!(*parsed)["include_hidden"].is_boolean()) {
      return std::unexpected(core::Error::invalid_argument("FileSearch: `include_hidden` must be a boolean"));
    }
    options.include_hidden = (*parsed)["include_hidden"].get<bool>();
  }

  if (parsed->contains("regex")) {
    if (!(*parsed)["regex"].is_boolean()) {
      return std::unexpected(core::Error::invalid_argument("FileSearch: `regex` must be a boolean"));
    }
    options.regex = (*parsed)["regex"].get<bool>();
  }

  if (parsed->contains("max_output_bytes")) {
    if (!(*parsed)["max_output_bytes"].is_number_integer() || (*parsed)["max_output_bytes"].is_number_float()) {
      return std::unexpected(
          core::Error::invalid_argument("FileSearch: `max_output_bytes` must be a positive integer"));
    }
    const auto raw = (*parsed)["max_output_bytes"].get<std::int64_t>();
    if (raw <= 0) {
      return std::unexpected(core::Error::invalid_argument("FileSearch: `max_output_bytes` must be a positive integer")
                                 .with("value", std::to_string(raw)));
    }
    options.max_output_bytes = static_cast<std::size_t>(raw);
  }

  if (parsed->contains("respect_ignore")) {
    if (!(*parsed)["respect_ignore"].is_boolean()) {
      return std::unexpected(core::Error::invalid_argument("FileSearch: `respect_ignore` must be a boolean"));
    }
    options.respect_ignore = (*parsed)["respect_ignore"].get<bool>();
  }
  if (parsed->contains("allow_outside_workspace") && !(*parsed)["allow_outside_workspace"].is_boolean()) {
    return std::unexpected(core::Error::invalid_argument("FileSearch: `allow_outside_workspace` must be a boolean"));
  }

  return options;
}

[[nodiscard]] bool looks_binary(std::string_view contents) {
  const auto probe = contents.substr(0, std::min<std::size_t>(contents.size(), kBinaryProbeBytes));
  return std::ranges::any_of(probe, [](char c) { return c == '\0'; });
}

[[nodiscard]] bool is_cancelled(const asio::cancellation_state& cancellation) noexcept {
  return cancellation.cancelled() != asio::cancellation_type::none;
}

/// Read an already-authorized regular file through its pinned descriptor,
/// capped at `kMaxFileBytes`. The file was opened via a no-follow `open_file`
/// beneath the root authority, so this never reopens a pathname. `display` is
/// used only for error context. Polls `cancellation` once per 8 KiB chunk so a
/// pathological multi-GB file aborts promptly when the agent loop is torn down.
[[nodiscard]] core::Result<std::string> read_handle_capped(const io::ReadOnlyFile& file,
                                                           std::string_view display,
                                                           const asio::cancellation_state& cancellation) {
  std::string contents;
  std::array<char, 8192> buffer{};
  while (true) {
    if (is_cancelled(cancellation)) {
      return std::unexpected(core::Error::cancelled());
    }
    errno = 0;
    const auto count = ::read(file.native_handle(), buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return std::unexpected(
          core::Error::io("FileSearch: failed while reading file").with("path", std::string{display}));
    }
    if (count == 0) {
      break;
    }
    const auto next_size = static_cast<std::uintmax_t>(contents.size()) + static_cast<std::uintmax_t>(count);
    if (next_size > kMaxFileBytes) {
      return std::unexpected(core::Error::invalid_argument("FileSearch: file exceeds max bytes")
                                 .with("path", std::string{display})
                                 .with("max_bytes", std::to_string(kMaxFileBytes)));
    }
    contents.append(buffer.data(), static_cast<std::size_t>(count));
  }
  return contents;
}

[[nodiscard]] constexpr std::size_t count_digits(std::size_t value) noexcept {
  std::size_t digits = 1;
  while (value >= 10U) {
    ++digits;
    value /= 10U;
  }
  return digits;
}

/// Exact rendered byte cost of one `path:line_number:text` row, including the
/// `\n` separator between matches (the first match pays zero separator cost,
/// so the caller passes `prepend_separator = !out.empty()`). Kept exact rather
/// than estimated so the agent can pin the byte cap against a precise budget
/// without slack — important when prompt-cache hit rate hinges on stable
/// output sizes.
[[nodiscard]] constexpr std::size_t rendered_cost(std::string_view file_path,
                                                  std::size_t line_number,
                                                  std::string_view text,
                                                  bool prepend_separator) noexcept {
  return file_path.size() + 1U + count_digits(line_number) + 1U + text.size() + (prepend_separator ? 1U : 0U);
}

/// Scans `contents` line-by-line via `matcher` and appends each match to
/// `out`. Stops the moment `out.size()` reaches `match_budget` (matches are
/// already in the bag) OR the moment a candidate's rendered byte cost would
/// push `accumulated_bytes` past `byte_budget`. The caller passes
/// `match_budget = max_matches + 1` so that hitting the match budget is a
/// reliable truncation signal: if the final tally exceeds `max_matches`, at
/// least one further match existed. The byte budget caps prompt size — see
/// spec 0011 v1.1 "Output cap on `FileSearch`". Returns the truncation
/// reason that fired (or `none` when scanning ran to EOF).
[[nodiscard]] TruncReason scan_text(std::string_view contents,
                                    const LineMatcher& matcher,
                                    std::string_view file_path,
                                    std::vector<Match>& out,
                                    std::size_t match_budget,
                                    std::size_t byte_budget,
                                    std::size_t& accumulated_bytes) {
  std::size_t line_number = 1;
  std::size_t cursor = 0;
  while (out.size() < match_budget) {
    const auto next_newline = contents.find('\n', cursor);
    const auto line_end = (next_newline == std::string_view::npos) ? contents.size() : next_newline;
    const auto line = contents.substr(cursor, line_end - cursor);
    if (matcher.matches(line)) {
      const auto cost = rendered_cost(file_path, line_number, line, !out.empty());
      if (accumulated_bytes + cost > byte_budget) {
        return TruncReason::bytes;
      }
      out.push_back(Match{
          .path = std::string{file_path},
          .line_number = line_number,
          .text = std::string{line},
      });
      accumulated_bytes += cost;
    }
    if (next_newline == std::string_view::npos) {
      break;
    }
    cursor = next_newline + 1;
    ++line_number;
  }
  return TruncReason::none;
}

[[nodiscard]] core::Result<LineMatcher> build_matcher(const SearchOptions& opts) {
  if (!opts.regex) {
    return LineMatcher{.literal = opts.pattern, .regex = nullptr};
  }

  const auto key = RegexCacheKey{.pattern = opts.pattern, .partial_line_match = true};
  const auto now = core::time::now_utc();
  {
    const std::scoped_lock lock{regex_pattern_cache_mutex()};
    if (auto* cached = regex_pattern_cache().get(key, now); cached != nullptr && *cached != nullptr) {
      return LineMatcher{.literal = {}, .regex = *cached};
    }
  }

  auto compiled = permission::InputPattern::compile(opts.pattern);
  if (!compiled) {
    auto err = core::Error::invalid_argument("FileSearch: invalid regex").with("pattern", opts.pattern);
    for (const auto& [key, value] : compiled.error().context()) {
      err.with(key, value);
    }
    return std::unexpected(std::move(err));
  }

  auto pattern =
      std::shared_ptr<const permission::InputPattern>{std::make_shared<permission::InputPattern>(std::move(*compiled))};
  {
    const std::scoped_lock lock{regex_pattern_cache_mutex()};
    regex_pattern_cache().put(key, pattern, now);
  }
  return LineMatcher{.literal = {}, .regex = std::move(pattern)};
}

/// Open the search target beneath an already-pinned workspace authority. The
/// anchored relative path may name the authority root itself (`"."`), a
/// directory, or a regular file; classification happens through the pinned
/// descriptors, never by re-stating the pathname.
[[nodiscard]] core::Result<SearchRoot> open_resolved_root(const io::DirectoryAuthority& authority,
                                                          const std::string& relative_path,
                                                          std::string base_absolute) {
  auto directory = authority.open_directory(io::AnchoredPath{
      .relative_path = relative_path,
      .symlink_policy = io::AnchoredSymlinkPolicy::allow_beneath,
  });
  if (directory) {
    return SearchRoot{.file = std::nullopt,
                      .directory = std::move(*directory),
                      .base_absolute = std::move(base_absolute)};
  }

  auto file = authority.open_file(io::AnchoredPath{
      .relative_path = relative_path,
      .symlink_policy = io::AnchoredSymlinkPolicy::allow_beneath,
  });
  if (file) {
    return SearchRoot{.file = std::move(*file), .directory = std::nullopt, .base_absolute = std::move(base_absolute)};
  }
  return std::unexpected(std::move(file).error());
}

/// Open a trusted (workspace-less) search target. Classification keeps the
/// legacy error shapes; the directory walk still descends through a pinned
/// authority so the tree cannot be redirected mid-walk, and a caller-named
/// file is opened beneath its own parent directory. This mode is the direct
/// registry-dispatch surface used by embedders and tests; workspace-confined
/// dispatch never reaches it.
[[nodiscard]] core::Result<SearchRoot> open_trusted_root(const std::string& path) {
  std::error_code stat_ec;
  const std::filesystem::path root{path};
  if (!std::filesystem::exists(root, stat_ec)) {
    if (stat_ec) {
      return std::unexpected(core::Error::io("FileSearch: failed to stat path")
                                 .with("path", path)
                                 .with("system_error", stat_ec.message()));
    }
    return std::unexpected(core::Error::not_found("FileSearch: path does not exist").with("path", path));
  }

  if (std::filesystem::is_directory(root, stat_ec)) {
    auto directory = io::DirectoryAuthority::open_trusted(path);
    if (!directory) {
      return std::unexpected(std::move(directory).error());
    }
    return SearchRoot{.file = std::nullopt, .directory = std::move(*directory), .base_absolute = path};
  }
  if (std::filesystem::is_regular_file(root, stat_ec)) {
    // Resolve the caller-named file first (a trusted single-file search may
    // name a symlink), then pin its parent and open the leaf without follow so
    // the handle cannot be redirected after classification.
    auto canonical = std::filesystem::weakly_canonical(root, stat_ec);
    if (stat_ec) {
      return std::unexpected(core::Error::io("FileSearch: failed to open file")
                                 .with("path", path)
                                 .with("system_error", stat_ec.message()));
    }
    auto parent = io::DirectoryAuthority::open_trusted(canonical.parent_path().string());
    if (!parent) {
      return std::unexpected(std::move(parent).error());
    }
    auto file = parent->open_file(io::AnchoredPath{
        .relative_path = canonical.filename().string(),
        .symlink_policy = io::AnchoredSymlinkPolicy::reject_all,
    });
    if (!file) {
      return std::unexpected(std::move(file).error());
    }
    return SearchRoot{.file = std::move(*file), .directory = std::nullopt, .base_absolute = path};
  }
  return std::unexpected(
      core::Error::invalid_argument("FileSearch: path is neither a regular file nor a directory").with("path", path));
}

[[nodiscard]] core::Result<SearchOutcome>
walk_and_scan(const SearchOptions& opts, const SearchRoot& root, const asio::cancellation_state& cancellation) {
  auto matcher = build_matcher(opts);
  if (!matcher) {
    return std::unexpected(std::move(matcher).error());
  }

  SearchOutcome outcome;
  const std::size_t match_budget = opts.max_matches + 1U;
  std::size_t accumulated_bytes = 0;

  const auto display_for = [&](std::string_view absolute) {
    return opts.workspace == nullptr ? std::string{absolute} : opts.workspace->display_path(absolute);
  };

  if (root.file.has_value()) {
    // Single-file mode does NOT apply the binary heuristic — the caller named
    // this file explicitly, so we trust their intent. The heuristic only
    // filters during recursive directory walks.
    const auto display = display_for(root.base_absolute);
    auto contents = read_handle_capped(*root.file, display, cancellation);
    if (!contents) {
      return std::unexpected(std::move(contents).error());
    }
    outcome.bytes_read += static_cast<std::uintmax_t>(contents->size());
    ++outcome.files_scanned;
    const auto reason = scan_text(*contents,
                                  *matcher,
                                  display,
                                  outcome.matches,
                                  match_budget,
                                  opts.max_output_bytes,
                                  accumulated_bytes);
    if (reason == TruncReason::bytes) {
      outcome.truncated = TruncReason::bytes;
    }
  } else if (root.directory.has_value()) {
    // Ignore/dotfile policy stays in the tool layer; the anchored walk only
    // supplies pinned entries. The filter is fed reconstructed absolute paths
    // (`base / relative_path`) so `.gitignore` scoping and built-in skip rules
    // stay byte-identical to the pre-migration `recursive_directory_iterator`
    // walk, while ignore files load through the pinned parent authorities the
    // walk hands the visitor.
    auto walk_filter = WorkspaceWalkFilter::create(*root.directory,
                                                   root.base_absolute,
                                                   WorkspaceWalkOptions{
                                                       .include_hidden = opts.include_hidden,
                                                       .respect_ignore = opts.respect_ignore,
                                                   });
    const std::filesystem::path base{root.base_absolute};
    io::WalkVisitor visitor = [&](const io::DirectoryAuthority& parent,
                                  const io::WalkEntry& entry) -> core::Result<io::WalkAction> {
      const auto absolute = (base / entry.relative_path).string();
      const bool is_directory = entry.kind == io::DirectoryEntryKind::directory;
      if (walk_filter.should_skip(parent, absolute, is_directory)) {
        return is_directory ? io::WalkAction::skip_subtree : io::WalkAction::proceed;
      }
      // Symlinks and non-regular entries are classified by the walk and never
      // scanned; descent already refuses to follow a symlinked directory.
      if (entry.kind != io::DirectoryEntryKind::regular_file) {
        return io::WalkAction::proceed;
      }

      auto file = parent.open_file(io::AnchoredPath{
          .relative_path = entry.name,
          .symlink_policy = io::AnchoredSymlinkPolicy::reject_all,
      });
      if (!file) {
        // Unreadable file during a walk: skip it (matches the legacy silent
        // skip of files that cannot be opened mid-tree).
        return io::WalkAction::proceed;
      }
      const auto display = display_for(absolute);
      auto contents = read_handle_capped(*file, display, cancellation);
      if (!contents) {
        if (contents.error().kind() == core::ErrorKind::cancelled) {
          return std::unexpected(std::move(contents).error());
        }
        return io::WalkAction::proceed;  // oversized / unreadable: silently skipped during a walk
      }
      outcome.bytes_read += static_cast<std::uintmax_t>(contents->size());
      if (!looks_binary(*contents)) {
        ++outcome.files_scanned;
        const auto reason = scan_text(*contents,
                                      *matcher,
                                      display,
                                      outcome.matches,
                                      match_budget,
                                      opts.max_output_bytes,
                                      accumulated_bytes);
        if (reason == TruncReason::bytes) {
          outcome.truncated = TruncReason::bytes;
        }
      }
      if (outcome.matches.size() >= match_budget || outcome.truncated == TruncReason::bytes) {
        return io::WalkAction::stop;
      }
      return io::WalkAction::proceed;
    };

    auto walked = io::walk_directory_tree(
        *root.directory,
        // Legacy parity with `std::filesystem::directory_options::
        // skip_permission_denied`: an unreadable subtree is pruned, not fatal.
        io::WalkTreeOptions{.max_entries = 0, .skip_permission_denied = true},
        [&cancellation] { return is_cancelled(cancellation); },
        visitor);
    if (!walked) {
      return std::unexpected(std::move(walked).error());
    }
  } else {
    return std::unexpected(core::Error::internal("FileSearch: search root produced no target"));
  }

  if (outcome.matches.size() > opts.max_matches) {
    // The match-count cap dominates the byte cap when both could have fired:
    // a caller who set a tight `max_matches` and a slack `max_output_bytes`
    // expects the "capped at N" message, not the byte one.
    outcome.truncated = TruncReason::matches;
    outcome.matches.resize(opts.max_matches);
  }
  return outcome;
}

[[nodiscard]] std::string render(const SearchOutcome& outcome, const SearchOptions& opts) {
  if (outcome.matches.empty() && outcome.truncated == TruncReason::none) {
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
  switch (outcome.truncated) {
    case TruncReason::none:
      break;
    case TruncReason::matches:
      if (!text.empty()) {
        text.push_back('\n');
      }
      std::format_to(std::back_inserter(text), "(truncated; matches capped at {})", opts.max_matches);
      break;
    case TruncReason::bytes:
      if (!text.empty()) {
        text.push_back('\n');
      }
      std::format_to(std::back_inserter(text), "(truncated; output capped at {} bytes)", opts.max_output_bytes);
      break;
  }
  return text;
}

/// Build the structured `Output::data_json` payload that mirrors the rendered
/// text but exposes match cardinality and per-walk usage to callers that no
/// longer want to parse the trailing summary line. The shape is intentionally
/// flat: every consumer (provider adapter, audit fan-out, desktop app) reads the
/// same JSON object and decides how to project it.
[[nodiscard]] std::string format_data_json(const SearchOutcome& outcome, const SearchOptions& opts) {
  nlohmann::json matches = nlohmann::json::array();
  for (const auto& m : outcome.matches) {
    matches.push_back(nlohmann::json{
        {"path", m.path},
        {"line_number", m.line_number},
        {"text", m.text},
    });
  }

  nlohmann::json truncation_reason = nullptr;
  switch (outcome.truncated) {
    case TruncReason::none:
      break;
    case TruncReason::matches:
      truncation_reason = "matches";
      break;
    case TruncReason::bytes:
      truncation_reason = "bytes";
      break;
  }

  return nlohmann::json{
      {"kind", "file_search"},
      {"path", opts.workspace == nullptr ? opts.path : opts.workspace->display_path(opts.path)},
      {"pattern", opts.pattern},
      {"regex", opts.regex},
      {"matches", std::move(matches)},
      {"match_count", outcome.matches.size()},
      {"truncated", outcome.truncated != TruncReason::none},
      {"truncation_reason", truncation_reason},
      {"files_scanned", outcome.files_scanned},
      {"bytes_read", outcome.bytes_read},
  }
      .dump();
}

[[nodiscard]] async::Awaitable<core::Result<Output>> file_search_handler(std::string_view input_json,
                                                                         DispatchContext& ctx) {
  auto opts = parse_input(input_json);
  if (!opts) {
    co_return std::unexpected(std::move(opts).error());
  }
  opts->workspace = ctx.workspace;

  // Open the pinned search root before the executor hop. Workspace dispatch
  // supplies the pre-resolved authority; the trusted no-workspace mode pins a
  // fresh root itself. The walk below never re-resolves the pathname.
  core::Result<SearchRoot> root = std::unexpected(core::Error::internal("FileSearch: search root was not resolved"));
  if (ctx.resolved_path.has_value()) {
    if (!ctx.resolved_path->authority.has_value()) {
      co_return std::unexpected(core::Error::internal("FileSearch: resolved workspace path is missing authority"));
    }
    opts->path = ctx.resolved_path->absolute_path;
    root = open_resolved_root(*ctx.resolved_path->authority,
                              ctx.resolved_path->authority_relative_path,
                              ctx.resolved_path->absolute_path);
  } else if (ctx.workspace != nullptr) {
    co_return std::unexpected(
        core::Error::internal("FileSearch: workspace dispatch did not provide a resolved authority"));
  } else {
    root = open_trusted_root(opts->path);
  }
  if (!root) {
    co_return std::unexpected(std::move(root).error());
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

  auto outcome = walk_and_scan(*opts, *root, cancellation);
  if (!outcome) {
    co_return std::unexpected(std::move(outcome).error());
  }
  auto text = render(*outcome, *opts);
  auto data_json = format_data_json(*outcome, *opts);
  const auto truncated = outcome->truncated != TruncReason::none;
  co_return Output{
      .text = std::move(text),
      .data_json = std::move(data_json),
      .usage =
          ToolUsage{
              .bytes_read = outcome->bytes_read,
              .files_touched = outcome->files_scanned,
              .match_count = static_cast<std::uint64_t>(outcome->matches.size()),
              .truncated = truncated,
          },
  };
}

}  // namespace

core::Result<void> register_file_search(Registry& registry) {
  core::ToolDef def{
      .name = std::string{kFileSearchName},
      .description = "Search a UTF-8 text file or (recursively) a directory for matches. "
                     "Input: {\"path\": <string>, \"pattern\": <string>, \"max_matches\"?: uint (default 100), "
                     "\"include_hidden\"?: bool (default false), \"regex\"?: bool (default false), "
                     "\"max_output_bytes\"?: uint (default 1048576), \"respect_ignore\"?: bool (default true), "
                     "\"allow_outside_workspace\"?: bool (default false; requires approval)}. "
                     "Default mode treats `pattern` as a literal substring (no-escape). When `regex=true`, "
                     "`pattern` is compiled as a re2 expression and matched against each line via partial match; "
                     "an invalid pattern returns `invalid_argument` with the re2 error attached. "
                     "Returns one `path:line:text` line per match; when a cap is hit a trailing "
                     "`(truncated; matches capped at <N>)` or `(truncated; output capped at <N> bytes)` line is "
                     "appended depending on which limit fired. Files containing NUL bytes in their first 8 KiB are "
                     "treated as binary and skipped during a directory walk. When `respect_ignore=true` (the "
                     "default), the recursive walk skips `.git`, `.xmake`, `.orangutan`, `build`, and "
                     "`node_modules` directories regardless of `include_hidden`, and honours `.gitignore` / "
                     "`.ignore` files from the search root downward for comments, blanks, escaped leading `#` / "
                     "`!` literals, `!` negation, trailing `/` directory rules, slash-relative patterns, basename "
                     "patterns, and fnmatch-style globs. "
                     "Returns the literal text `no "
                     "matches` (non-error) when nothing matched. Successful calls also fill `data_json` with "
                     "kind, path, pattern, regex, matches[], match_count, truncated, truncation_reason, "
                     "files_scanned, and bytes_read; `usage` reports `bytes_read`, `files_touched`, "
                     "`match_count`, and the `truncated` cap flag. When a Workspace is supplied, output paths use "
                     "stable display labels such as `<workspace>/src/main.cpp` instead of raw absolute paths.",
      .input_schema_json = std::string{kFileSearchSchema},
      .required_capabilities = {core::Capability::read_file},
      .deferred = false,
      .category = "file",
  };
  return registry.add(std::move(def), &file_search_handler);
}

}  // namespace orangutan::tool
