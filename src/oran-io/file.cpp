// src/oran-io/file.cpp — file and directory helper implementation.

#include <oran/io/file.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <asio/cancellation_type.hpp>
#include <asio/post.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <oran/core/bounded_cache.hpp>
#include <oran/core/error.hpp>
#include <oran/core/time.hpp>
#include <oran/io/fingerprint.hpp>

namespace orangutan::io {

namespace {

constexpr std::size_t kReadChunkSize = 8192;
constexpr std::uintmax_t kMidReadRetryThresholdBytes = 64U * 1024U;
constexpr std::uintmax_t kLineOffsetIndexThresholdBytes = 256U * 1024U;
constexpr std::size_t kLineOffsetIndexMaxEntries = 32U;
constexpr std::size_t kLineOffsetIndexMaxBytes = 8U * 1024U * 1024U;
constexpr std::size_t kFileViewCacheMaxEntries = 64U;
constexpr std::size_t kFileViewCacheMaxBytes = 16U * 1024U * 1024U;
constexpr std::size_t kReadTextSingleflightMaxEntries = 64U;

template <typename T>
[[nodiscard]] std::size_t hash_combine(std::size_t seed, const T& value) noexcept {
  const auto h = std::hash<T>{}(value);
  return seed ^ (h + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U));
}

struct LineOffsetIndexKey {
  std::string canonical_path;
  std::uintmax_t size_bytes{0};
  std::uint64_t mtime_ns{0};

  friend bool operator==(const LineOffsetIndexKey&, const LineOffsetIndexKey&) = default;
};

struct LineOffsetIndexKeyHash {
  [[nodiscard]] std::size_t operator()(const LineOffsetIndexKey& key) const noexcept {
    auto seed = std::hash<std::string>{}(key.canonical_path);
    seed = hash_combine(seed, key.size_bytes);
    return hash_combine(seed, key.mtime_ns);
  }
};

struct LineOffsetIndex {
  std::uintmax_t file_size{0};
  std::vector<std::uintmax_t> line_starts;
};

struct LineOffsetIndexByteCost {
  [[nodiscard]] std::size_t operator()(const std::shared_ptr<const LineOffsetIndex>& index) const noexcept {
    return index == nullptr ? 0 : sizeof(LineOffsetIndex) + index->line_starts.size() * sizeof(std::uintmax_t);
  }
};

using LineOffsetIndexCache = core::BoundedCache<LineOffsetIndexKey,
                                                std::shared_ptr<const LineOffsetIndex>,
                                                LineOffsetIndexByteCost,
                                                LineOffsetIndexKeyHash>;

enum class FileViewRangeKind : std::uint8_t {
  whole,
  lines,
  bytes,
};

struct FileViewCacheKey {
  std::string canonical_path;
  std::uintmax_t size_bytes{0};
  std::uint64_t mtime_ns{0};
  std::uintmax_t max_bytes{0};
  FileViewRangeKind range_kind{FileViewRangeKind::whole};
  std::uintmax_t range_start{0};
  std::uintmax_t range_length{0};

  friend bool operator==(const FileViewCacheKey&, const FileViewCacheKey&) = default;
};

struct FileViewCacheKeyHash {
  [[nodiscard]] std::size_t operator()(const FileViewCacheKey& key) const noexcept {
    auto seed = std::hash<std::string>{}(key.canonical_path);
    seed = hash_combine(seed, key.size_bytes);
    seed = hash_combine(seed, key.mtime_ns);
    seed = hash_combine(seed, key.max_bytes);
    seed = hash_combine(seed, static_cast<std::uint8_t>(key.range_kind));
    seed = hash_combine(seed, key.range_start);
    return hash_combine(seed, key.range_length);
  }
};

struct ReadTextResultByteCost {
  [[nodiscard]] std::size_t operator()(const ReadTextResult& result) const noexcept {
    return result.text.size();
  }
};

using FileViewCache =
    core::BoundedCache<FileViewCacheKey, ReadTextResult, ReadTextResultByteCost, FileViewCacheKeyHash>;

struct PreparedReadTextFile {
  std::optional<ReadTextResult> ready;
  FileFingerprint fingerprint;
  FileViewCacheKey cache_key;
};

struct ReadTextSingleflightEntry {
  std::optional<core::Result<ReadTextResult>> result;
  std::vector<std::shared_ptr<asio::steady_timer>> waiters;
};

struct ReadTextSingleflightJoin {
  std::shared_ptr<ReadTextSingleflightEntry> entry;
  std::shared_ptr<asio::steady_timer> waiter;
  bool leader{false};
  bool bypass{false};
};

using ReadTextSingleflightTable =
    std::unordered_map<FileViewCacheKey, std::shared_ptr<ReadTextSingleflightEntry>, FileViewCacheKeyHash>;

[[nodiscard]] LineOffsetIndexCache& line_offset_index_cache() {
  static auto cache = LineOffsetIndexCache{
      LineOffsetIndexCache::Options{
          .max_entries = kLineOffsetIndexMaxEntries,
          .max_bytes = kLineOffsetIndexMaxBytes,
          .ttl = std::chrono::minutes{10},
      },
      LineOffsetIndexByteCost{},
  };
  return cache;
}

[[nodiscard]] FileViewCache& file_view_cache() {
  static auto cache = FileViewCache{
      FileViewCache::Options{
          .max_entries = kFileViewCacheMaxEntries,
          .max_bytes = kFileViewCacheMaxBytes,
          .ttl = std::chrono::minutes{10},
      },
      ReadTextResultByteCost{},
  };
  return cache;
}

[[nodiscard]] ReadTextSingleflightTable& read_text_singleflight_table() {
  static auto table = ReadTextSingleflightTable{};
  return table;
}

[[nodiscard]] ReadTextSingleflightStats& read_text_singleflight_stats_mutable() {
  static auto stats = ReadTextSingleflightStats{};
  return stats;
}

[[nodiscard]] std::mutex& line_offset_index_mutex() {
  static std::mutex mutex;
  return mutex;
}

[[nodiscard]] std::mutex& file_view_cache_mutex() {
  static std::mutex mutex;
  return mutex;
}

[[nodiscard]] std::mutex& read_text_singleflight_mutex() {
  static std::mutex mutex;
  return mutex;
}

[[nodiscard]] bool is_cancelled(const asio::cancellation_state& cancellation) noexcept {
  return cancellation.cancelled() != asio::cancellation_type::none;
}

[[nodiscard]] bool is_hidden(std::string_view name) noexcept {
  return !name.empty() && name.front() == '.';
}

[[nodiscard]] std::filesystem::path to_path(const std::string& path) {
  return std::filesystem::path{path};
}

[[nodiscard]] std::string cache_key_path(const std::string& path) {
  std::error_code ec;
  auto canonical = std::filesystem::weakly_canonical(to_path(path), ec);
  if (!ec) {
    return canonical.generic_string();
  }
  canonical = std::filesystem::absolute(to_path(path), ec);
  if (!ec) {
    return canonical.lexically_normal().generic_string();
  }
  return to_path(path).lexically_normal().generic_string();
}

[[nodiscard]] LineOffsetIndexKey line_offset_index_key(const std::string& path, const FileFingerprint& fingerprint) {
  return LineOffsetIndexKey{
      .canonical_path = cache_key_path(path),
      .size_bytes = fingerprint.size_bytes,
      .mtime_ns = fingerprint.mtime_ns,
  };
}

[[nodiscard]] FileViewCacheKey
file_view_cache_key(const std::string& path, const FileFingerprint& fingerprint, const ReadTextOptions& options) {
  FileViewCacheKey key{
      .canonical_path = cache_key_path(path),
      .size_bytes = fingerprint.size_bytes,
      .mtime_ns = fingerprint.mtime_ns,
      .max_bytes = options.max_bytes,
  };
  if (options.range && options.range->lines) {
    key.range_kind = FileViewRangeKind::lines;
    key.range_start = static_cast<std::uintmax_t>(options.range->lines->start_line);
    key.range_length = static_cast<std::uintmax_t>(options.range->lines->line_count);
  } else if (options.range && options.range->bytes) {
    key.range_kind = FileViewRangeKind::bytes;
    key.range_start = options.range->bytes->offset_bytes;
    key.range_length = options.range->bytes->length_bytes;
  }
  return key;
}

void clear_line_offset_index_cache() {
  const std::scoped_lock lock{line_offset_index_mutex()};
  line_offset_index_cache().clear();
}

void clear_file_view_cache() {
  const std::scoped_lock lock{file_view_cache_mutex()};
  file_view_cache().clear();
}

void clear_file_caches() {
  clear_line_offset_index_cache();
  clear_file_view_cache();
}

[[nodiscard]] std::optional<ReadTextResult> get_cached_file_view(const FileViewCacheKey& key) {
  const auto now = core::time::now_utc();
  const std::scoped_lock lock{file_view_cache_mutex()};
  if (auto* cached = file_view_cache().get(key, now); cached != nullptr) {
    return *cached;
  }
  return std::nullopt;
}

void put_cached_file_view(FileViewCacheKey key, const ReadTextResult& result) {
  const auto now = core::time::now_utc();
  const std::scoped_lock lock{file_view_cache_mutex()};
  file_view_cache().put(std::move(key), result, now);
}

[[nodiscard]] ReadTextSingleflightJoin join_read_text_singleflight(const FileViewCacheKey& key,
                                                                   asio::any_io_executor executor) {
  const std::scoped_lock lock{read_text_singleflight_mutex()};
  auto& table = read_text_singleflight_table();
  auto& stats = read_text_singleflight_stats_mutable();

  if (auto existing = table.find(key); existing != table.end()) {
    auto waiter = std::make_shared<asio::steady_timer>(std::move(executor));
    waiter->expires_at(std::chrono::steady_clock::time_point::max());
    existing->second->waiters.push_back(waiter);
    ++stats.followers_joined;
    return ReadTextSingleflightJoin{
        .entry = existing->second,
        .waiter = std::move(waiter),
        .leader = false,
        .bypass = false,
    };
  }

  if (table.size() >= kReadTextSingleflightMaxEntries) {
    ++stats.bypassed_capacity;
    return ReadTextSingleflightJoin{
        .entry = {},
        .waiter = {},
        .leader = false,
        .bypass = true,
    };
  }

  auto entry = std::make_shared<ReadTextSingleflightEntry>();
  table.emplace(key, entry);
  ++stats.leaders_started;
  return ReadTextSingleflightJoin{
      .entry = std::move(entry),
      .waiter = {},
      .leader = true,
      .bypass = false,
  };
}

void detach_read_text_singleflight_waiter(const std::shared_ptr<ReadTextSingleflightEntry>& entry,
                                          const std::shared_ptr<asio::steady_timer>& waiter) {
  const std::scoped_lock lock{read_text_singleflight_mutex()};
  if (!entry->result) {
    const auto removed = std::ranges::remove(entry->waiters, waiter);
    entry->waiters.erase(removed.begin(), removed.end());
  }
}

void complete_read_text_singleflight(const FileViewCacheKey& key,
                                     const std::shared_ptr<ReadTextSingleflightEntry>& entry,
                                     const core::Result<ReadTextResult>& result) {
  std::vector<std::shared_ptr<asio::steady_timer>> waiters;
  {
    const std::scoped_lock lock{read_text_singleflight_mutex()};
    entry->result = result;
    auto& table = read_text_singleflight_table();
    if (auto existing = table.find(key); existing != table.end() && existing->second == entry) {
      table.erase(existing);
    }

    auto& stats = read_text_singleflight_stats_mutable();
    ++stats.completions;
    if (!result) {
      ++stats.errors;
    }
    waiters.swap(entry->waiters);
  }

  for (const auto& waiter : waiters) {
    waiter->expires_at(std::chrono::steady_clock::time_point::min());
  }
}

[[nodiscard]] std::optional<core::Result<ReadTextResult>>
read_text_singleflight_result(const std::shared_ptr<ReadTextSingleflightEntry>& entry) {
  const std::scoped_lock lock{read_text_singleflight_mutex()};
  return entry->result;
}

[[nodiscard]] core::Error io_error(std::string message, const std::string& path) {
  return core::Error::io(std::move(message)).with("path", path);
}

[[nodiscard]] core::Error system_io_error(std::string message, const std::string& path, const std::error_code& ec) {
  auto error = [&] {
    if (ec == std::errc::file_exists) {
      return core::Error{core::ErrorKind::conflict, std::move(message)};
    }
    if (ec == std::errc::permission_denied) {
      return core::Error::permission_denied(std::move(message));
    }
    if (ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory) {
      return core::Error::not_found(std::move(message));
    }
    return core::Error::io(std::move(message));
  }();
  return error.with("path", path).with("system_error", ec.message()).with("category", ec.category().name());
}

[[nodiscard]] core::Error errno_io_error(std::string message, const std::string& path) {
  return system_io_error(std::move(message), path, std::error_code{errno, std::generic_category()});
}

[[nodiscard]] core::Error stream_open_error(std::string message, const std::string& path) {
  if (errno != 0) {
    return errno_io_error(std::move(message), path);
  }
  return io_error(std::move(message), path);
}

[[nodiscard]] core::Result<void> validate_path(std::string_view path) {
  if (path.empty()) {
    return std::unexpected(core::Error::invalid_argument("path must not be empty"));
  }
  return {};
}

[[nodiscard]] core::Result<void> ensure_readable_regular_file(const std::string& path) {
  auto fs_path = to_path(path);
  std::error_code ec;
  if (!std::filesystem::exists(fs_path, ec)) {
    if (ec) {
      return std::unexpected(system_io_error("failed to stat file", path, ec));
    }
    return std::unexpected(core::Error::not_found("file does not exist").with("path", path));
  }

  if (!std::filesystem::is_regular_file(fs_path, ec)) {
    if (ec) {
      return std::unexpected(system_io_error("failed to inspect file type", path, ec));
    }
    return std::unexpected(core::Error::invalid_argument("path is not a regular file").with("path", path));
  }
  return {};
}

/// Adjust a buffer to UTF-8 code-point boundaries at both ends. The function
/// drops leading bytes that are continuation bytes (0x80..0xBF) without a
/// matching lead, and trims any trailing partial multi-byte sequence whose
/// declared length runs past the buffer end. Invalid lead bytes inside the
/// buffer stop the scan — the prefix up to the last good boundary is kept.
/// Returns `[head, end)` of the aligned slice; the caller resizes its
/// owning string when this differs from the current size.
[[nodiscard]] std::pair<std::size_t, std::size_t> align_to_utf8_boundaries(std::string_view buffer) noexcept {
  std::size_t head = 0;
  while (head < buffer.size()) {
    const auto b = static_cast<std::uint8_t>(buffer[head]);
    if ((b & 0xC0) != 0x80) {
      break;
    }
    ++head;
  }
  std::size_t end = head;
  std::size_t i = head;
  while (i < buffer.size()) {
    const auto b = static_cast<std::uint8_t>(buffer[i]);
    int length = 0;
    if (b < 0x80) {
      length = 1;
    } else if (b < 0xC2) {
      break;
    } else if (b < 0xE0) {
      length = 2;
    } else if (b < 0xF0) {
      length = 3;
    } else if (b < 0xF5) {
      length = 4;
    } else {
      break;
    }
    if (i + static_cast<std::size_t>(length) > buffer.size()) {
      break;
    }
    i += static_cast<std::size_t>(length);
    end = i;
  }
  return {head, end};
}

[[nodiscard]] core::Result<void> validate_range(const FileRange& range) {
  const bool has_lines = range.lines.has_value();
  const bool has_bytes = range.bytes.has_value();
  if (has_lines == has_bytes) {
    return std::unexpected(core::Error::invalid_argument("FileRange must specify exactly one of lines or bytes"));
  }
  if (has_lines) {
    if (range.lines->start_line == 0 || range.lines->line_count == 0) {
      return std::unexpected(core::Error::invalid_argument("line range fields must be non-zero"));
    }
    return {};
  }
  if (range.bytes->offset_bytes == 0 || range.bytes->length_bytes == 0) {
    return std::unexpected(core::Error::invalid_argument("byte range fields must be non-zero"));
  }
  return {};
}

/// Stream the entire file into `out`, stopping at `max_bytes` and setting
/// `truncated`. `out` is cleared on entry; the count of trailing newlines
/// determines `end_line` so a whole-file read can populate the 1-based
/// line span without a second pass after the read returns.
[[nodiscard]] core::Result<void>
read_whole_file_into(const std::string& path, std::uintmax_t max_bytes, std::string& out, bool& truncated) {
  out.clear();
  truncated = false;
  errno = 0;
  std::ifstream input{to_path(path), std::ios::binary};
  if (!input) {
    return std::unexpected(stream_open_error("failed to open file for reading", path));
  }

  std::array<char, kReadChunkSize> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count <= 0) {
      break;
    }
    const auto available = max_bytes - static_cast<std::uintmax_t>(out.size());
    const auto take = std::min<std::uintmax_t>(static_cast<std::uintmax_t>(count), available);
    if (take > 0) {
      out.append(buffer.data(), static_cast<std::size_t>(take));
    }
    if (static_cast<std::uintmax_t>(count) > take) {
      truncated = true;
      break;
    }
  }
  if (!truncated && input.bad()) {
    return std::unexpected(io_error("failed while reading file", path));
  }
  // Trim a UTF-8 multi-byte tail that the byte cap may have split. Whole-file
  // reads that fit inside `max_bytes` are unaffected because the truncation
  // never fired.
  if (truncated && !out.empty()) {
    const auto [head, end] = align_to_utf8_boundaries(out);
    if (head > 0 || end < out.size()) {
      out = out.substr(head, end - head);
    }
  }
  return {};
}

[[nodiscard]] std::uint64_t count_line_span(std::string_view text) noexcept {
  if (text.empty()) {
    return 0;
  }
  const auto newlines = static_cast<std::uint64_t>(std::ranges::count(text, '\n'));
  return newlines + (text.back() == '\n' ? 0 : 1);
}

[[nodiscard]] core::Result<std::shared_ptr<const LineOffsetIndex>>
build_line_offset_index(const std::string& path, const FileFingerprint& fingerprint) {
  errno = 0;
  std::ifstream input{to_path(path), std::ios::binary};
  if (!input) {
    return std::unexpected(stream_open_error("failed to open file for line-offset indexing", path));
  }

  auto index = std::make_shared<LineOffsetIndex>();
  index->file_size = fingerprint.size_bytes;
  if (fingerprint.size_bytes > 0) {
    index->line_starts.push_back(0);
  }

  std::uintmax_t absolute_offset = 0;
  std::array<char, kReadChunkSize> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count <= 0) {
      break;
    }
    for (std::streamsize i = 0; i < count; ++i) {
      const auto next_offset = absolute_offset + 1U;
      if (buffer[static_cast<std::size_t>(i)] == '\n' && next_offset < fingerprint.size_bytes) {
        index->line_starts.push_back(next_offset);
      }
      absolute_offset = next_offset;
    }
  }
  if (input.bad()) {
    return std::unexpected(io_error("failed while building line-offset index", path));
  }
  return std::shared_ptr<const LineOffsetIndex>{std::move(index)};
}

[[nodiscard]] core::Result<std::shared_ptr<const LineOffsetIndex>>
get_line_offset_index(const std::string& path, const FileFingerprint& fingerprint) {
  const auto key = line_offset_index_key(path, fingerprint);
  const auto now = core::time::now_utc();
  {
    const std::scoped_lock lock{line_offset_index_mutex()};
    if (auto* cached = line_offset_index_cache().get(key, now); cached != nullptr && *cached != nullptr) {
      return *cached;
    }
  }

  auto built = build_line_offset_index(path, fingerprint);
  if (!built) {
    return std::unexpected(std::move(built).error());
  }

  {
    const std::scoped_lock lock{line_offset_index_mutex()};
    line_offset_index_cache().put(key, *built, now);
  }
  return *built;
}

[[nodiscard]] core::Result<ReadTextResult> read_whole_file_blocking(const std::string& path, std::uintmax_t max_bytes) {
  ReadTextResult result;
  if (auto ok = read_whole_file_into(path, max_bytes, result.text, result.truncated); !ok) {
    return std::unexpected(std::move(ok).error());
  }
  result.returned_bytes = static_cast<std::uintmax_t>(result.text.size());
  result.start_line = 1;
  const auto lines = count_line_span(result.text);
  result.end_line = lines;
  return result;
}

[[nodiscard]] core::Result<ReadTextResult>
read_line_range_blocking(const std::string& path, FileRange::LineSpan lines, std::uintmax_t max_bytes) {
  errno = 0;
  std::ifstream input{to_path(path), std::ios::binary};
  if (!input) {
    return std::unexpected(stream_open_error("failed to open file for reading", path));
  }

  ReadTextResult result;
  result.start_line = lines.start_line;
  result.end_line = lines.start_line - 1;  // empty span by default
  std::uint64_t current_line = 1;
  std::uint64_t emitted_lines = 0;
  std::array<char, kReadChunkSize> buffer{};

  while (input && emitted_lines < lines.line_count) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count <= 0) {
      break;
    }
    for (std::streamsize i = 0; i < count; ++i) {
      const char c = buffer[static_cast<std::size_t>(i)];
      const bool in_range = current_line >= lines.start_line && emitted_lines < lines.line_count;
      if (in_range) {
        if (static_cast<std::uintmax_t>(result.text.size()) >= max_bytes) {
          result.truncated = true;
          input.setstate(std::ios::eofbit);
          break;
        }
        result.text.push_back(c);
      }
      if (c == '\n') {
        if (in_range) {
          ++emitted_lines;
          result.end_line = lines.start_line + emitted_lines - 1;
        }
        ++current_line;
      }
    }
    if (result.truncated) {
      break;
    }
  }
  if (!result.truncated && input.bad()) {
    return std::unexpected(io_error("failed while reading file", path));
  }

  // The final line of a range that ends at EOF may lack a trailing newline.
  // Bump `end_line` so the reported span covers the partial last line.
  if (!result.text.empty() && result.text.back() != '\n' && emitted_lines < lines.line_count) {
    ++emitted_lines;
    result.end_line = lines.start_line + emitted_lines - 1;
  }

  if (result.truncated && !result.text.empty()) {
    const auto [head, end] = align_to_utf8_boundaries(result.text);
    if (head > 0 || end < result.text.size()) {
      result.text = result.text.substr(head, end - head);
    }
  }

  result.returned_bytes = static_cast<std::uintmax_t>(result.text.size());
  return result;
}

[[nodiscard]] core::Result<ReadTextResult> read_line_range_with_index(const std::string& path,
                                                                      FileRange::LineSpan lines,
                                                                      std::uintmax_t max_bytes,
                                                                      const LineOffsetIndex& index) {
  ReadTextResult result;
  result.start_line = lines.start_line;
  result.end_line = lines.start_line - 1;

  const auto start_index = lines.start_line - 1U;
  if (start_index >= index.line_starts.size()) {
    return result;
  }

  const auto remaining_lines = static_cast<std::uint64_t>(index.line_starts.size()) - start_index;
  const auto line_count = std::min(lines.line_count, remaining_lines);
  const auto end_index = start_index + line_count;
  const auto start_offset = index.line_starts[static_cast<std::size_t>(start_index)];
  const auto end_offset =
      end_index < index.line_starts.size() ? index.line_starts[static_cast<std::size_t>(end_index)] : index.file_size;
  const auto requested_bytes = end_offset > start_offset ? end_offset - start_offset : std::uintmax_t{0};
  const auto capped_bytes = std::min<std::uintmax_t>(requested_bytes, max_bytes);

  errno = 0;
  std::ifstream input{to_path(path), std::ios::binary};
  if (!input) {
    return std::unexpected(stream_open_error("failed to open file for indexed line-range read", path));
  }
  input.seekg(static_cast<std::streamoff>(start_offset));
  if (!input) {
    return std::unexpected(io_error("failed to seek indexed line range", path));
  }

  std::array<char, kReadChunkSize> buffer{};
  while (input && static_cast<std::uintmax_t>(result.text.size()) < capped_bytes) {
    const auto remaining = capped_bytes - static_cast<std::uintmax_t>(result.text.size());
    const auto chunk = std::min<std::uintmax_t>(static_cast<std::uintmax_t>(buffer.size()), remaining);
    input.read(buffer.data(), static_cast<std::streamsize>(chunk));
    const auto count = input.gcount();
    if (count <= 0) {
      break;
    }
    result.text.append(buffer.data(), static_cast<std::size_t>(count));
  }
  if (input.bad()) {
    return std::unexpected(io_error("failed while reading indexed line range", path));
  }

  if (max_bytes < requested_bytes && static_cast<std::uintmax_t>(result.text.size()) >= max_bytes) {
    result.truncated = true;
  }
  if (result.truncated && !result.text.empty()) {
    const auto [head, end] = align_to_utf8_boundaries(result.text);
    if (head > 0 || end < result.text.size()) {
      result.text = result.text.substr(head, end - head);
    }
  }

  result.returned_bytes = static_cast<std::uintmax_t>(result.text.size());
  const auto returned_lines = count_line_span(result.text);
  if (returned_lines > 0) {
    result.end_line = lines.start_line + returned_lines - 1U;
  }
  return result;
}

[[nodiscard]] core::Result<ReadTextResult> read_line_range_dispatch(const std::string& path,
                                                                    FileRange::LineSpan lines,
                                                                    std::uintmax_t max_bytes,
                                                                    const FileFingerprint& fingerprint) {
  if (fingerprint.size_bytes <= kLineOffsetIndexThresholdBytes) {
    return read_line_range_blocking(path, lines, max_bytes);
  }
  auto index = get_line_offset_index(path, fingerprint);
  if (!index) {
    return std::unexpected(std::move(index).error());
  }
  return read_line_range_with_index(path, lines, max_bytes, **index);
}

[[nodiscard]] core::Result<ReadTextResult>
read_byte_range_blocking(const std::string& path, FileRange::ByteSpan bytes, std::uintmax_t max_bytes) {
  errno = 0;
  std::ifstream input{to_path(path), std::ios::binary};
  if (!input) {
    return std::unexpected(stream_open_error("failed to open file for reading", path));
  }

  ReadTextResult result;
  result.start_line = 1;
  result.end_line = 0;

  const auto seek_to = static_cast<std::streamoff>(bytes.offset_bytes);
  input.seekg(seek_to);
  if (!input) {
    // Seek past EOF is not an error; return an empty range and let the caller
    // observe `returned_bytes == 0`. Clear eof/fail bits so the bad-bit check
    // below stays honest.
    input.clear();
    result.returned_bytes = 0;
    return result;
  }

  const auto cap = std::min<std::uintmax_t>(bytes.length_bytes, max_bytes);
  std::array<char, kReadChunkSize> buffer{};
  while (input && static_cast<std::uintmax_t>(result.text.size()) < cap) {
    const auto remaining = cap - static_cast<std::uintmax_t>(result.text.size());
    const auto chunk = std::min<std::uintmax_t>(static_cast<std::uintmax_t>(buffer.size()), remaining);
    input.read(buffer.data(), static_cast<std::streamsize>(chunk));
    const auto count = input.gcount();
    if (count <= 0) {
      break;
    }
    result.text.append(buffer.data(), static_cast<std::size_t>(count));
  }
  if (input.bad()) {
    return std::unexpected(io_error("failed while reading file", path));
  }

  // `truncated` fires when the output cap (`max_bytes`) cut the requested
  // length short — not when the caller-supplied `length_bytes` exceeds EOF.
  if (max_bytes < bytes.length_bytes && static_cast<std::uintmax_t>(result.text.size()) >= max_bytes) {
    result.truncated = true;
  }

  // Adjust both ends of the buffer to UTF-8 code-point boundaries: byte
  // ranges can land mid-codepoint at the start (the user's `offset_bytes`
  // chops a multi-byte sequence) AND at the end (`length_bytes` truncates
  // a trailing sequence). `align_to_utf8_boundaries` drops both ranges so
  // the returned text is always valid UTF-8 when the underlying file is.
  if (!result.text.empty()) {
    const auto [head, end] = align_to_utf8_boundaries(result.text);
    if (head > 0 || end < result.text.size()) {
      result.text = result.text.substr(head, end - head);
    }
  }
  result.returned_bytes = static_cast<std::uintmax_t>(result.text.size());
  return result;
}

[[nodiscard]] core::Result<ReadTextResult>
dispatch_read(const std::string& path, const ReadTextOptions& options, const FileFingerprint& fingerprint) {
  try {
    if (!options.range) {
      return read_whole_file_blocking(path, options.max_bytes);
    }
    if (options.range->lines) {
      return read_line_range_dispatch(path, *options.range->lines, options.max_bytes, fingerprint);
    }
    return read_byte_range_blocking(path, *options.range->bytes, options.max_bytes);
  } catch (const std::filesystem::filesystem_error& e) {
    return std::unexpected(system_io_error("filesystem read failed", path, e.code()));
  } catch (const std::exception& e) {
    return std::unexpected(io_error("file read failed", path).with("exception", e.what()));
  }
}

[[nodiscard]] core::Result<PreparedReadTextFile> prepare_read_text_file_blocking(const std::string& path,
                                                                                 const ReadTextOptions& options) {
  if (auto valid = validate_path(path); !valid) {
    return std::unexpected(valid.error());
  }
  if (options.max_bytes == 0) {
    return std::unexpected(core::Error::invalid_argument("max_bytes must be greater than zero").with("path", path));
  }
  if (options.range) {
    if (auto ok = validate_range(*options.range); !ok) {
      return std::unexpected(std::move(ok).error().with("path", path));
    }
  }
  if (auto regular = ensure_readable_regular_file(path); !regular) {
    return std::unexpected(regular.error());
  }

  auto pre = compute_file_fingerprint(path);
  if (!pre) {
    return std::unexpected(std::move(pre).error());
  }

  auto cache_key = file_view_cache_key(path, *pre, options);
  if (auto cached = get_cached_file_view(cache_key)) {
    auto post = compute_file_fingerprint(path);
    if (!post) {
      return std::unexpected(std::move(post).error());
    }
    if (*pre == *post) {
      cached->fingerprint = *post;
      return PreparedReadTextFile{
          .ready = *cached,
          .fingerprint = *post,
          .cache_key = std::move(cache_key),
      };
    }
    pre = post;
    cache_key = file_view_cache_key(path, *pre, options);
  }

  return PreparedReadTextFile{
      .ready = std::nullopt,
      .fingerprint = *pre,
      .cache_key = std::move(cache_key),
  };
}

[[nodiscard]] core::Result<ReadTextResult> read_text_file_cold_blocking(const std::string& path,
                                                                        const ReadTextOptions& options,
                                                                        const FileFingerprint& fingerprint,
                                                                        const FileViewCacheKey& cache_key) {
  // Mid-read race detection: capture the fingerprint before and after the
  // blocking read. Size or mtime drift either retries (small whole-file
  // reads) or surfaces as `Error::conflict` (large or ranged reads).
  auto pre = fingerprint;
  auto first = dispatch_read(path, options, pre);
  if (!first) {
    return std::unexpected(std::move(first).error());
  }

  auto post = compute_file_fingerprint(path);
  if (!post) {
    return std::unexpected(std::move(post).error());
  }

  if (pre != *post) {
    const bool ranged = options.range.has_value();
    const bool large = pre.size_bytes >= kMidReadRetryThresholdBytes;
    if (ranged || large) {
      return std::unexpected(core::Error{core::ErrorKind::conflict, "file changed during read"}
                                 .with("path", path)
                                 .with("size_before", std::to_string(pre.size_bytes))
                                 .with("size_after", std::to_string(post->size_bytes)));
    }
    // Small whole-file read: retry once.
    pre = *post;
    first = dispatch_read(path, options, pre);
    if (!first) {
      return std::unexpected(std::move(first).error());
    }
    post = compute_file_fingerprint(path);
    if (!post) {
      return std::unexpected(std::move(post).error());
    }
    if (pre != *post) {
      return std::unexpected(
          core::Error{core::ErrorKind::conflict, "file changed during read (after retry)"}.with("path", path));
    }
  }

  first->fingerprint = *post;
  const auto result_key = (*post == fingerprint) ? cache_key : file_view_cache_key(path, *post, options);
  put_cached_file_view(result_key, *first);
  return first;
}

[[nodiscard]] core::Result<void> create_parent_directories(const std::filesystem::path& fs_path,
                                                           const std::string& original_path) {
  const auto parent = fs_path.parent_path();
  if (parent.empty()) {
    return {};
  }

  std::error_code ec;
  std::filesystem::create_directories(parent, ec);
  if (ec) {
    return std::unexpected(system_io_error("failed to create parent directories", original_path, ec));
  }
  return {};
}

/// Sibling temp path used by the atomic-write fast path. Lives in the target's
/// parent directory so `std::filesystem::rename` stays on a single filesystem
/// (and therefore atomic under POSIX rename(2)). Uniqueness comes from a
/// process-local atomic counter — concurrent callers each pull a fresh
/// sequence number, so two threads racing for the same final path never share
/// a temp leaf. The leading `.` keeps the temp out of LLM-facing directory
/// listings that default to hiding dotfiles.
[[nodiscard]] std::filesystem::path atomic_temp_path(const std::filesystem::path& target) {
  static std::atomic<std::uint64_t> counter{0U};
  const auto seq = counter.fetch_add(1U, std::memory_order_relaxed);
  auto leaf = std::format(".{}.orangutan.tmp.{}", target.filename().string(), seq);
  const auto parent = target.parent_path();
  return parent.empty() ? std::filesystem::path{leaf} : parent / leaf;
}

[[nodiscard]] core::Result<void> stream_write(const std::filesystem::path& fs_path,
                                              const std::string& original_path,
                                              const std::string& contents,
                                              std::ios::openmode mode) {
  errno = 0;
  std::ofstream output{fs_path, mode};
  if (!output) {
    return std::unexpected(stream_open_error("failed to open file for writing", original_path));
  }
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!output) {
    return std::unexpected(io_error("failed while writing file", original_path));
  }
  // Explicit flush + close so a failure is observed before we rename the
  // temp into place; the destructor swallows errors on close otherwise.
  output.close();
  if (!output) {
    return std::unexpected(io_error("failed while closing file", original_path));
  }
  return {};
}

[[nodiscard]] core::Result<void> atomic_write_blocking(const std::filesystem::path& target,
                                                       const std::string& original_path,
                                                       const std::string& contents) {
  const auto temp = atomic_temp_path(target);
  if (auto written = stream_write(temp, original_path, contents, std::ios::binary | std::ios::out | std::ios::trunc);
      !written) {
    std::error_code cleanup_ec;
    std::filesystem::remove(temp, cleanup_ec);
    return std::unexpected(std::move(written).error());
  }

  std::error_code rename_ec;
  std::filesystem::rename(temp, target, rename_ec);
  if (rename_ec) {
    std::error_code cleanup_ec;
    std::filesystem::remove(temp, cleanup_ec);
    return std::unexpected(system_io_error("failed to commit atomic write", original_path, rename_ec));
  }
  return {};
}

[[nodiscard]] core::Result<void>
write_text_file_blocking(const std::string& path, const std::string& contents, WriteTextOptions options) {
  if (auto valid = validate_path(path); !valid) {
    return std::unexpected(valid.error());
  }
  if (static_cast<std::uintmax_t>(contents.size()) >
      static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
    return std::unexpected(core::Error::invalid_argument("contents too large to write").with("path", path));
  }
  if (options.atomic && options.mode != WriteMode::truncate) {
    return std::unexpected(
        core::Error::invalid_argument("atomic write requires WriteMode::truncate").with("path", path));
  }

  try {
    const auto fs_path = to_path(path);
    if (options.create_parent_directories) {
      if (auto created = create_parent_directories(fs_path, path); !created) {
        return std::unexpected(created.error());
      }
    }

    std::error_code ec;
    if (options.mode == WriteMode::fail_if_exists && std::filesystem::exists(fs_path, ec)) {
      if (ec) {
        return std::unexpected(system_io_error("failed to check destination", path, ec));
      }
      return std::unexpected(core::Error{core::ErrorKind::conflict, "file already exists"}.with("path", path));
    }

    if (options.atomic) {
      auto written = atomic_write_blocking(fs_path, path, contents);
      if (written) {
        clear_file_caches();
      }
      return written;
    }

    auto mode = std::ios::binary | std::ios::out;
    if (options.mode == WriteMode::append) {
      mode |= std::ios::app;
    } else if (options.mode == WriteMode::fail_if_exists) {
      mode |= std::ios::trunc | std::ios::noreplace;
    } else {
      mode |= std::ios::trunc;
    }

    auto written = stream_write(fs_path, path, contents, mode);
    if (written) {
      clear_file_caches();
    }
    return written;
  } catch (const std::filesystem::filesystem_error& e) {
    return std::unexpected(system_io_error("filesystem write failed", path, e.code()));
  } catch (const std::exception& e) {
    return std::unexpected(io_error("file write failed", path).with("exception", e.what()));
  }
}

[[nodiscard]] DirectoryEntryKind classify(const std::filesystem::file_status& status) noexcept {
  if (std::filesystem::is_symlink(status)) {
    return DirectoryEntryKind::symlink;
  }
  if (std::filesystem::is_regular_file(status)) {
    return DirectoryEntryKind::regular_file;
  }
  if (std::filesystem::is_directory(status)) {
    return DirectoryEntryKind::directory;
  }
  return DirectoryEntryKind::other;
}

[[nodiscard]] std::optional<std::uintmax_t> regular_file_size(const std::filesystem::directory_entry& entry) {
  std::error_code ec;
  if (!entry.is_regular_file(ec) || ec) {
    return std::nullopt;
  }
  const auto size = entry.file_size(ec);
  if (ec) {
    return std::nullopt;
  }
  return size;
}

[[nodiscard]] core::Result<std::vector<DirectoryEntry>> list_directory_blocking(const std::string& path,
                                                                                ListDirectoryOptions options) {
  if (auto valid = validate_path(path); !valid) {
    return std::unexpected(valid.error());
  }
  if (options.max_entries == 0) {
    return std::unexpected(core::Error::invalid_argument("max_entries must be greater than zero").with("path", path));
  }

  try {
    const auto fs_path = to_path(path);
    std::error_code ec;
    if (!std::filesystem::exists(fs_path, ec)) {
      if (ec) {
        return std::unexpected(system_io_error("failed to stat directory", path, ec));
      }
      return std::unexpected(core::Error::not_found("directory does not exist").with("path", path));
    }
    if (!std::filesystem::is_directory(fs_path, ec)) {
      if (ec) {
        return std::unexpected(system_io_error("failed to inspect directory type", path, ec));
      }
      return std::unexpected(core::Error::invalid_argument("path is not a directory").with("path", path));
    }

    std::vector<DirectoryEntry> entries;
    auto it =
        std::filesystem::directory_iterator{fs_path, std::filesystem::directory_options::skip_permission_denied, ec};
    if (ec) {
      return std::unexpected(system_io_error("failed to open directory", path, ec));
    }

    for (const auto& entry : it) {
      const auto name = entry.path().filename().string();
      if (!options.include_hidden && is_hidden(name)) {
        continue;
      }
      if (entries.size() >= options.max_entries) {
        return std::unexpected(core::Error::io("directory entry limit exceeded")
                                   .with("path", path)
                                   .with("max_entries", std::to_string(options.max_entries)));
      }

      std::error_code status_ec;
      const auto status = entry.symlink_status(status_ec);
      if (status_ec) {
        return std::unexpected(system_io_error("failed to inspect directory entry", entry.path().string(), status_ec));
      }
      const auto kind = classify(status);
      entries.push_back(DirectoryEntry{
          .name = name,
          .path = entry.path().string(),
          .kind = kind,
          .size_bytes = kind == DirectoryEntryKind::regular_file ? regular_file_size(entry) : std::nullopt,
      });
    }

    std::ranges::sort(entries, {}, &DirectoryEntry::path);
    return entries;
  } catch (const std::filesystem::filesystem_error& e) {
    return std::unexpected(system_io_error("filesystem directory listing failed", path, e.code()));
  } catch (const std::exception& e) {
    return std::unexpected(io_error("directory listing failed", path).with("exception", e.what()));
  }
}

[[nodiscard]] core::Result<void> delete_file_blocking(const std::string& path) {
  if (auto valid = validate_path(path); !valid) {
    return std::unexpected(valid.error());
  }

  try {
    const auto fs_path = to_path(path);
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(fs_path, ec);
    if (ec) {
      return std::unexpected(system_io_error("failed to stat file", path, ec));
    }
    if (!std::filesystem::status_known(status) || status.type() == std::filesystem::file_type::not_found) {
      return std::unexpected(core::Error::not_found("file does not exist").with("path", path));
    }
    if (!std::filesystem::is_regular_file(status)) {
      return std::unexpected(core::Error::invalid_argument("path is not a regular file").with("path", path));
    }

    if (!std::filesystem::remove(fs_path, ec)) {
      if (ec) {
        return std::unexpected(system_io_error("failed to delete file", path, ec));
      }
      // remove() returned false without an error_code: the file vanished
      // between the stat and the unlink — surface it as a race-condition
      // not_found so the caller sees a consistent end state.
      return std::unexpected(core::Error::not_found("file disappeared before delete").with("path", path));
    }
    clear_file_caches();
    return {};
  } catch (const std::filesystem::filesystem_error& e) {
    return std::unexpected(system_io_error("filesystem delete failed", path, e.code()));
  } catch (const std::exception& e) {
    return std::unexpected(io_error("file delete failed", path).with("exception", e.what()));
  }
}

template <typename ResultT, typename Fn>
[[nodiscard]] async::Awaitable<ResultT> run_blocking(asio::any_io_executor executor, Fn fn) {
  auto cancellation = co_await asio::this_coro::cancellation_state;
  if (is_cancelled(cancellation)) {
    co_return std::unexpected(core::Error::cancelled());
  }

  co_await asio::post(std::move(executor), asio::use_awaitable);
  if (is_cancelled(cancellation)) {
    co_return std::unexpected(core::Error::cancelled());
  }

  co_return fn();
}

[[nodiscard]] async::Awaitable<core::Result<PreparedReadTextFile>>
prepare_read_text_file_async(asio::any_io_executor executor, std::string path, ReadTextOptions options) {
  co_return co_await run_blocking<core::Result<PreparedReadTextFile>>(
      std::move(executor),
      [path = std::move(path), options] { return prepare_read_text_file_blocking(path, options); });
}

[[nodiscard]] async::Awaitable<core::Result<ReadTextResult>> read_text_file_cold_async(asio::any_io_executor executor,
                                                                                       std::string path,
                                                                                       ReadTextOptions options,
                                                                                       FileFingerprint fingerprint,
                                                                                       FileViewCacheKey cache_key) {
  co_return co_await run_blocking<core::Result<ReadTextResult>>(
      std::move(executor),
      [path = std::move(path), options, fingerprint = std::move(fingerprint), cache_key = std::move(cache_key)] {
        return read_text_file_cold_blocking(path, options, fingerprint, cache_key);
      });
}

[[nodiscard]] async::Awaitable<core::Result<ReadTextResult>>
wait_for_read_text_singleflight(ReadTextSingleflightJoin join) {
  asio::error_code ec;
  co_await join.waiter->async_wait(asio::redirect_error(asio::use_awaitable, ec));

  auto result = read_text_singleflight_result(join.entry);
  if (!result) {
    detach_read_text_singleflight_waiter(join.entry, join.waiter);
    co_return std::unexpected(core::Error::cancelled());
  }
  co_return *std::move(result);
}

}  // namespace

async::Awaitable<core::Result<std::string>>
read_text_file(asio::any_io_executor executor, std::string path, ReadTextOptions options) {
  const auto original_path = path;
  auto rich = co_await read_text_file_ranged(std::move(executor), std::move(path), options);
  if (!rich) {
    co_return std::unexpected(std::move(rich).error());
  }
  if (rich->truncated) {
    co_return std::unexpected(core::Error::invalid_argument("file exceeds max_bytes")
                                  .with("path", original_path)
                                  .with("max_bytes", std::to_string(options.max_bytes)));
  }
  co_return std::move(rich->text);
}

async::Awaitable<core::Result<ReadTextResult>>
read_text_file_ranged(asio::any_io_executor executor, std::string path, ReadTextOptions options) {
  auto prepared = co_await prepare_read_text_file_async(executor, path, options);
  if (!prepared) {
    co_return std::unexpected(std::move(prepared).error());
  }
  if (prepared->ready) {
    co_return std::move(*prepared->ready);
  }

  auto cache_key = prepared->cache_key;
  auto fingerprint = prepared->fingerprint;
  auto join = join_read_text_singleflight(cache_key, executor);
  if (!join.leader && !join.bypass) {
    co_return co_await wait_for_read_text_singleflight(std::move(join));
  }

  if (join.leader) {
    co_await asio::post(executor, asio::use_awaitable);
  }
  auto result = co_await read_text_file_cold_async(std::move(executor),
                                                   std::move(path),
                                                   options,
                                                   std::move(fingerprint),
                                                   cache_key);
  if (join.leader) {
    complete_read_text_singleflight(cache_key, join.entry, result);
  }
  co_return result;
}

ReadTextSingleflightStats read_text_file_ranged_singleflight_stats() {
  const std::scoped_lock lock{read_text_singleflight_mutex()};
  auto stats = read_text_singleflight_stats_mutable();
  const auto& table = read_text_singleflight_table();
  stats.current_in_flight = table.size();
  stats.current_waiters =
      std::transform_reduce(table.begin(), table.end(), std::size_t{0}, std::plus<>{}, [](const auto& item) {
        return item.second->waiters.size();
      });
  return stats;
}

async::Awaitable<core::Result<void>>
write_text_file(asio::any_io_executor executor, std::string path, std::string contents, WriteTextOptions options) {
  co_return co_await run_blocking<core::Result<void>>(
      std::move(executor),
      [path = std::move(path), contents = std::move(contents), options] {
        return write_text_file_blocking(path, contents, options);
      });
}

async::Awaitable<core::Result<std::vector<DirectoryEntry>>>
list_directory(asio::any_io_executor executor, std::string path, ListDirectoryOptions options) {
  co_return co_await run_blocking<core::Result<std::vector<DirectoryEntry>>>(
      std::move(executor),
      [path = std::move(path), options] { return list_directory_blocking(path, options); });
}

async::Awaitable<core::Result<void>> delete_file(asio::any_io_executor executor, std::string path) {
  co_return co_await run_blocking<core::Result<void>>(std::move(executor),
                                                      [path = std::move(path)] { return delete_file_blocking(path); });
}

}  // namespace orangutan::io
