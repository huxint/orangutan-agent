// include/oran/io/file.hpp — coroutine file and directory helpers.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <asio/any_io_executor.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/io/range.hpp>

namespace orangutan::io {

class ReadOnlyFile;
class FileMutation;
class DeleteMutation;
class DirectoryAuthority;

enum class WriteMode : std::uint8_t {
  truncate,
  append,
  fail_if_exists,
};

enum class WriteTextDurability : std::uint8_t {
  rename_only,
  fsync_file,
  fsync_file_and_parent,
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

struct DeletePathOptions {
  /// Directory deletion requires explicit recursion intent. Regular files
  /// delete regardless of this flag; a symlink target path is refused.
  bool recursive{false};
};

struct DeletePathResult {
  /// Number of directory entries removed, including a recursively deleted
  /// root directory.
  std::uintmax_t paths_removed{0};
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
  /// Durability policy for atomic writes. `rename_only` preserves the fast
  /// temp-then-rename behavior. `fsync_file` fsyncs the staged file before the
  /// rename; `fsync_file_and_parent` also fsyncs the parent directory after a
  /// successful rename so the directory entry is durable. Non-default
  /// durability requires `atomic == true`.
  WriteTextDurability durability{WriteTextDurability::rename_only};
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

/// Range-aware read from an already-authorized regular file. This overload
/// never reopens the diagnostic pathname and intentionally bypasses the
/// pathname-keyed caches; the held descriptor is the file identity.
[[nodiscard]] async::Awaitable<core::Result<ReadTextResult>>
read_text_file_ranged(asio::any_io_executor executor, ReadOnlyFile file, ReadTextOptions options = {});

/// Invalidate every process-local `read_text_file_ranged` cache entry for
/// `path`. The path is canonicalised through the same private key helper as
/// reads; no cache keys or file contents are exposed. This is the public seam
/// for explicit callers and watcher callbacks.
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

/// Write through a pinned directory authority. Truncate stages a sibling
/// temporary file and revalidates the target identity immediately before
/// rename; append and fail-if-exists remain anchored to the pinned parent.
[[nodiscard]] async::Awaitable<core::Result<void>> write_text_file(asio::any_io_executor executor,
                                                                   FileMutation mutation,
                                                                   std::string contents,
                                                                   WriteTextOptions options = {});

[[nodiscard]] async::Awaitable<core::Result<std::vector<DirectoryEntry>>>
list_directory(asio::any_io_executor executor, std::string path, ListDirectoryOptions options = {});

/// Enumerate one pinned directory without reopening its diagnostic pathname.
/// Entry metadata is read relative to the directory descriptor and symlinks
/// are classified without being followed.
[[nodiscard]] async::Awaitable<core::Result<std::vector<DirectoryEntry>>>
list_directory(asio::any_io_executor executor, DirectoryAuthority directory, ListDirectoryOptions options = {});

/// One entry surfaced during an anchored tree walk. `relative_path` is the
/// forward-slash path from the walk root (the root itself is never visited).
/// `depth` is 1 for direct children of the root and increases by one per level.
/// Directory entries are visited before their contents so a visitor can decide
/// whether to descend; symlinks are classified here but never followed.
struct WalkEntry {
  std::string name;
  std::string relative_path;
  DirectoryEntryKind kind{DirectoryEntryKind::other};
  std::optional<std::uintmax_t> size_bytes;
  std::size_t depth{0};
};

/// A visitor's decision for one `WalkEntry`.
enum class WalkAction : std::uint8_t {
  /// Keep walking. For a directory entry this descends into it; for a
  /// non-directory it simply continues to the next sibling.
  proceed,
  /// Do not descend into this directory (ignored for non-directories);
  /// continue with the next sibling.
  skip_subtree,
  /// Stop the whole walk immediately and return successfully.
  stop,
};

/// Per-entry visitor invoked on the blocking walk. The pinned parent authority
/// for the entry is supplied so the visitor can open a matched file through an
/// anchored `open_file` without reopening any pathname. The visitor returns a
/// `WalkAction` to steer descent, or an error to abort the walk with that error.
/// Policy (ignore rules, hidden-name filtering, entry caps) lives in the
/// visitor; this primitive supplies only pinned entries and never applies a
/// filter of its own beyond skipping `.`/`..`.
using WalkVisitor =
    std::move_only_function<core::Result<WalkAction>(const DirectoryAuthority& parent, const WalkEntry& entry)>;

struct WalkTreeOptions {
  /// Hard bound on entries handed to the visitor. Exceeding it aborts the walk
  /// with `Error::io` (`reason=walk_entry_limit`). `0` disables the bound.
  std::size_t max_entries{0};
  /// Mirror of `std::filesystem::directory_options::skip_permission_denied`
  /// for callers that prefer pruning over failing: when true, a subdirectory
  /// whose contents cannot be enumerated because of filesystem permissions is
  /// skipped (its own entry was already offered to the visitor; its subtree is
  /// not visited) instead of aborting the walk. The walk root itself still
  /// surfaces `permission_denied`.
  bool skip_permission_denied{false};
};

/// Walk the tree beneath a pinned root `DirectoryAuthority` depth-first,
/// invoking `visitor` for every entry. Descent goes exclusively through
/// dirfd-relative `open_directory` (no-follow), so a symlinked directory is
/// classified and offered to the visitor but never traversed, and no pathname
/// is reopened mid-walk. `cancelled` is polled once per entry; a `true` result
/// aborts with `Error::cancelled`. The walk is synchronous — callers hop the
/// blocking executor themselves (e.g. via `run_blocking`) exactly as the other
/// authority helpers do.
[[nodiscard]] core::Result<void> walk_directory_tree(const DirectoryAuthority& root,
                                                     WalkTreeOptions options,
                                                     const std::function<bool()>& cancelled,
                                                     WalkVisitor& visitor);

/// Consume a pinned delete capability. Regular files delete directly;
/// directories require `options.recursive=true` and are walked relative to
/// pinned descriptors without following symlinks. Changes observed by the
/// final identity check return `conflict`; POSIX does not provide a conditional
/// unlink, so an external writer can still race the final check-to-unlink gap.
[[nodiscard]] async::Awaitable<core::Result<DeletePathResult>>
delete_path(asio::any_io_executor executor, DeleteMutation mutation, DeletePathOptions options = {});

}  // namespace orangutan::io
