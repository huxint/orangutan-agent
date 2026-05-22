// include/oran/io/file.hpp — coroutine file and directory helpers.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <asio/any_io_executor.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/io/range.hpp>

namespace orangutan::io {

enum class WriteMode : std::uint8_t {
  truncate,
  append,
  fail_if_exists,
};

struct ReadTextOptions {
  std::uintmax_t max_bytes{16U * 1024U * 1024U};
  /// Optional line- or byte-range request. When unset the helper returns
  /// the entire file (subject to `max_bytes`). See `FileRange` for the
  /// mutual-exclusion contract; invalid ranges reject with
  /// `Error::invalid_argument`.
  std::optional<FileRange> range{};
};

/// Snapshot of one bounded cache used by `read_text_file_ranged`.
/// Lifetime counters are monotonic and survive cache clears; `current_*`
/// fields describe the cache at snapshot time. Keys and paths are not
/// exposed.
struct ReadTextBoundedCacheStats {
  std::uint64_t hits{0};
  std::uint64_t misses{0};
  std::uint64_t evictions_lru{0};
  std::uint64_t evictions_ttl{0};
  std::uint64_t evictions_bytes{0};
  std::uint64_t rejected_oversize{0};
  std::size_t current_entries{0};
  std::size_t current_bytes{0};
};

/// Process-local cache stats for the line-offset index and file-view cache
/// behind `read_text_file_ranged`.
struct ReadTextFileCacheStats {
  ReadTextBoundedCacheStats line_offset_index;
  ReadTextBoundedCacheStats file_view;
};

/// Options for the path-stale watcher that invalidates
/// `read_text_file_ranged` caches when filesystem events arrive.
struct ReadTextFileWatchOptions {
  bool recursive{true};
  /// Maximum number of inotify events to process before returning. `0`
  /// means "run until cancelled" and is the production shape; tests can set
  /// a small value to drain a deterministic batch.
  std::size_t max_events{0};
};

/// Aggregate watcher result. Paths and cache keys are intentionally not
/// exposed; this is operational health only.
struct ReadTextFileWatchStats {
  std::size_t directories_watched{0};
  std::uint64_t events_seen{0};
  std::uint64_t invalidations{0};
};

/// Process-local singleflight stats for `read_text_file_ranged`. The
/// numbers expose bounded-state health without leaking cache keys or file
/// paths. Lifetime counters are monotonic; `current_*` fields describe the
/// in-flight table at the instant this snapshot was taken.
struct ReadTextSingleflightStats {
  std::uint64_t leaders_started{0};
  std::uint64_t followers_joined{0};
  std::uint64_t bypassed_capacity{0};
  std::uint64_t completions{0};
  std::uint64_t errors{0};
  std::size_t current_in_flight{0};
  std::size_t current_waiters{0};
};

struct WriteTextOptions {
  WriteMode mode{WriteMode::truncate};
  bool create_parent_directories{false};
  /// When set, write `contents` to a sibling temp file in the target's parent
  /// directory and `std::filesystem::rename` it into place — the POSIX
  /// rename(2) on the same filesystem is atomic, so a crash or partial write
  /// leaves the original target intact instead of truncated. Requires
  /// `mode == WriteMode::truncate`; the append / fail_if_exists modes are
  /// incompatible with the temp-then-rename pattern and return
  /// `invalid_argument`.
  bool atomic{false};
};

enum class DirectoryEntryKind : std::uint8_t {
  regular_file,
  directory,
  symlink,
  other,
};

struct DirectoryEntry {
  std::string name;
  std::string path;
  DirectoryEntryKind kind{DirectoryEntryKind::other};
  std::optional<std::uintmax_t> size_bytes;
};

struct ListDirectoryOptions {
  bool include_hidden{false};
  std::size_t max_entries{4096};
};

[[nodiscard]] async::Awaitable<core::Result<std::string>>
read_text_file(asio::any_io_executor executor, std::string path, ReadTextOptions options = {});

/// Range-aware read returning a `ReadTextResult` with fingerprint, span,
/// returned-byte count, and a `truncated` flag. The blocking impl
/// captures a `FileFingerprint` before and after the read; size or mtime
/// drift signals a mid-read race and is either retried once (whole-file
/// reads of files < 64 KiB) or surfaced as `Error::conflict` (larger or
/// ranged reads). Spec 0011 v1 (file-view system) is the contract.
[[nodiscard]] async::Awaitable<core::Result<ReadTextResult>>
read_text_file_ranged(asio::any_io_executor executor, std::string path, ReadTextOptions options = {});

/// Invalidate every process-local `read_text_file_ranged` cache entry for
/// `path`. The path is canonicalised through the same private key helper as
/// reads; no cache keys or file contents are exposed. This is the public seam
/// for in-process mutations and watcher callbacks.
void invalidate_read_text_file_ranged_cache(std::string_view path);

/// Watch `root` for local filesystem changes and invalidate
/// `read_text_file_ranged` cache entries for changed paths. On Linux this is
/// backed by inotify via asio descriptors. The default shape runs until the
/// caller cancels the coroutine; `max_events` is available for bounded test
/// and one-shot drains.
[[nodiscard]] async::Awaitable<core::Result<ReadTextFileWatchStats>>
watch_read_text_file_ranged_cache(asio::any_io_executor executor,
                                  std::string root,
                                  ReadTextFileWatchOptions options = {});

/// Snapshot the process-local bounded caches used by
/// `read_text_file_ranged`. This is an observability hook only; callers
/// cannot mutate or inspect private cache keys.
[[nodiscard]] ReadTextFileCacheStats read_text_file_ranged_cache_stats();

/// Snapshot the process-local singleflight table used by
/// `read_text_file_ranged`. This is an observability hook only; callers
/// cannot mutate or inspect the private in-flight keys.
[[nodiscard]] ReadTextSingleflightStats read_text_file_ranged_singleflight_stats();

[[nodiscard]] async::Awaitable<core::Result<void>>
write_text_file(asio::any_io_executor executor, std::string path, std::string contents, WriteTextOptions options = {});

[[nodiscard]] async::Awaitable<core::Result<std::vector<DirectoryEntry>>>
list_directory(asio::any_io_executor executor, std::string path, ListDirectoryOptions options = {});

/// Delete the regular file at `path`. Refuses directories and symlinks with
/// `invalid_argument` — the v1 surface is deliberately narrow so an LLM-driven
/// delete cannot recursively destroy a tree or unlink a symlink to a directory
/// outside the workspace. Returns `not_found` when no file exists at `path`.
/// The future direction for filesystem mutation is consolidation into a single
/// delete helper that handles files AND folders (with recursion intent
/// expressed by the caller), not separate per-kind helpers.
[[nodiscard]] async::Awaitable<core::Result<void>> delete_file(asio::any_io_executor executor, std::string path);

}  // namespace orangutan::io
