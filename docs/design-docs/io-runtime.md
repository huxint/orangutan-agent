# IO Runtime

`oran-io` is the policy-free platform library for local file-system and process I/O.
It sits below tools, skills, hooks, channels, and bootstrap: those higher layers decide
whether an action is allowed and which lifecycle event to publish; `oran-io` only
performs the requested operation and returns `core::Result<T>`.

> **Slice-2 status (2026-05-15):** `oran-io` ships text-file read/write helpers and
> deterministic directory listing. Subprocesses, pipes, signals, glob expansion,
> and permission/hook integration are planned future slices; watcher-backed
> range-read cache invalidation lands later in slices 57-58.
>
> **Slice-30 status (2026-05-20):** `oran-io` adds `delete_file(executor, path)`
> for regular-file removal. Directories and symlinks reject as
> `invalid_argument` so the v1 surface cannot be used to recursively
> destroy a tree or unlink a symlink that points outside the workspace.
>
> **Slice-265 status (2026-06-28):** `delete_path(executor, path, options)`
> is the unified delete helper for files and folders. Regular files delete
> directly, directory deletion requires `DeletePathOptions::recursive=true`,
> symlinks always reject, and the result reports the removed path count.
> `delete_file` remains as the regular-file compatibility wrapper.
> Recursive directory deletion conservatively invalidates all private
> range-read caches so removed child-file views cannot survive the mutation.
>
> **Slice-32 status (2026-05-21):** `WriteTextOptions` grows an opt-in
> `atomic` flag. When set on a `WriteMode::truncate` write, the helper
> stages the contents in a sibling `.<name>.orangutan.tmp.<seq>` and
> commits via `std::filesystem::rename` — atomic on POSIX same-filesystem
> rename(2), so a crash or partial write leaves the original target
> intact instead of truncated. The flag rejects `append` and
> `fail_if_exists` with `invalid_argument` (temp-then-rename has no
> coherent semantics for either). The tool layer wires `FileEdit`
> through the atomic path on every rewrite, and `FileWrite` through
> the atomic path whenever `mode == truncate`; the deep-review BUG-4.1.1
> data-loss footgun is closed.
>
> **Slice-153 status (2026-06-04):** atomic writes now expose an explicit
> `WriteTextDurability` policy. The default `rename_only` preserves the
> existing fast temp-then-rename behavior; `fsync_file` fsyncs the staged temp
> file before rename, and `fsync_file_and_parent` also fsyncs the parent
> directory after a successful rename. Durability modes reject unless
> `atomic=true`, and cache invalidation is tied to successful rename so a
> post-rename parent fsync failure cannot leave stale in-process file views.
> Atomic temp leaves now use `.<name>.orangutan.tmp.<pid>.<random>` plus
> exclusive create/retry instead of a process-local counter, avoiding
> cross-process temp collisions.
>
> **Authority refactor (2026-07-12):** `DirectoryAuthority` can begin a
> move-only `FileMutation` from an authority-relative name. Mutation resolution
> always rejects symlink components; unlike read resolution, it exposes no
> permissive symlink-policy input. The mutation pins the target parent directory
> and any snapshotted inode, then records the existing regular file or its absence.
> Workspace-backed truncate writes stage a sibling temp, revalidate that
> snapshot's device, inode, size, mtime, and ctime immediately before `renameat`, and return
> `conflict/reason=stale_fingerprint` if another writer won. Append and
> fail-if-exists preserve their public semantics through the same pinned
> parent. This metadata guard narrows same-inode races but is not a content
> identity check or filesystem CAS; the underlying timestamp granularity and
> final validation-to-rename window remain acknowledged limits.
>
> **Slice-43 status (2026-05-22):** `oran-io` adds the range-aware
> `read_text_file_ranged(executor, path, options)` returning
> `ReadTextResult { text, fingerprint, start_line, end_line,
> returned_bytes, truncated }`. `ReadTextOptions` grows an optional
> `range: FileRange` (mutually exclusive `LineSpan { start_line,
> line_count }` or `ByteSpan { offset_bytes, length_bytes }`; both
> populated branches reject as `invalid_argument`). The blocking impl
> captures an `io::FileFingerprint` before AND after the read; size or
> mtime drift either retries once (whole-file reads of files < 64 KiB)
> or surfaces as `Error::conflict` with `path` / `size_before` /
> `size_after` context. Byte ranges align both ends to UTF-8 code-point
> boundaries so a `length_bytes` cap that lands mid-codepoint shrinks
> the returned text rather than smuggling invalid bytes downstream;
> the legacy `read_text_file` becomes a thin wrapper that calls the
> ranged path and returns `Error::invalid_argument` when the rich
> result reports `truncated=true`.
>
> **Slice-50 status (2026-05-23):** line-range reads of files larger
> than 256 KiB now use a lazy in-memory line-offset index. The index
> maps 1-based line numbers to byte offsets, lives in a bounded
> `core::BoundedCache` (32 entries / 8 MiB / 10-minute TTL), and is
> keyed by canonical path plus the cheap `(size_bytes, mtime_ns)`
> fingerprint. `write_text_file` and file deletes invalidate the
> matching canonical path after successful mutations so an agent cannot
> reuse stale offsets after an in-process write/delete.
>
> **Slice-52 status (2026-05-24):** `read_text_file_ranged` now keeps a
> bounded in-memory file-view cache for successful reads. Entries are keyed
> by canonical path, range, `max_bytes`, and the cheap
> `(size_bytes, mtime_ns)` fingerprint, capped at 64 entries / 16 MiB /
> 10 minutes, and revalidated with `stat` before every hit. Successful
> `write_text_file` and file delete calls invalidate the matching
> canonical path in both the file-view cache and the line-offset index
> synchronously.
>
> **Slice-53 status (2026-05-24):** concurrent cold
> `read_text_file_ranged` calls now share a bounded process-local
> singleflight table. Calls with the same canonical path, requested range,
> max-bytes budget, and cheap fingerprint collapse behind one leader read;
> followers await the same result. Hot file-view cache hits return before
> touching the table. `read_text_file_ranged_singleflight_stats()` exposes
> lifetime counters and current table size without leaking keys or paths.
>
> **Slice-54 status (2026-05-24):** the range-read cache observability
> surface now covers the two bounded caches as well as singleflight:
> `read_text_file_ranged_cache_stats()` snapshots the private line-offset
> index and file-view cache hit/miss/eviction/current-size counters without
> exposing keys or paths.
>
> **Slice-57 status (2026-05-24):** `oran-io` exposes
> `invalidate_read_text_file_ranged_cache(path)` as the public path-stale
> seam for future watcher callbacks. Successful `write_text_file` and
> regular-file deletes reuse the same seam, so in-process mutations evict
> only entries for the affected canonical path instead of clearing unrelated
> range-read cache entries; recursive directory deletes clear the private
> range-read caches because every child path under the removed tree is stale.
>
> **Slice-58 status (2026-05-24):** `oran-io` now ships the concrete
> Linux/inotify watcher event source for those caches:
> `watch_read_text_file_ranged_cache(executor, root, options)` registers one
> directory or a recursive tree, waits through an asio POSIX descriptor, and
> invalidates the changed event path through
> `invalidate_read_text_file_ranged_cache(path)`. The public result is
> aggregate health only (`ReadTextFileWatchStats`). If inotify reports a
> queue overflow, the watcher conservatively invalidates all private
> range-read caches. Slice 253 (2026-06-18) wires this as a runtime service:
> `orangutan --serve` (`bootstrap::run_serve`) auto-starts the watcher over the
> workspace root and cancels it gracefully on SIGINT/SIGTERM.

## Public Surface

```cpp
namespace orangutan::io {

enum class WriteMode { truncate, append, fail_if_exists };

struct ReadTextOptions {
  std::uintmax_t max_bytes{16U * 1024U * 1024U};
  // Slice 43: optional mutually-exclusive line- or byte-range request.
  std::optional<FileRange> range{};
};

struct FileRange {
  struct LineSpan { std::uint64_t start_line; std::uint64_t line_count; };
  struct ByteSpan { std::uintmax_t offset_bytes; std::uintmax_t length_bytes; };
  std::optional<LineSpan> lines;
  std::optional<ByteSpan> bytes;
};

struct ReadTextResult {
  std::string text;
  FileFingerprint fingerprint;
  std::uint64_t start_line{1};
  std::uint64_t end_line{0};
  std::uintmax_t returned_bytes{0};
  bool truncated{false};
};

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

struct ReadTextFileCacheStats {
  ReadTextBoundedCacheStats line_offset_index;
  ReadTextBoundedCacheStats file_view;
};

struct ReadTextFileWatchOptions {
  bool recursive{true};
  std::size_t max_events{0};  // 0 means run until cancelled.
};

struct ReadTextFileWatchStats {
  std::size_t directories_watched{0};
  std::uint64_t events_seen{0};
  std::uint64_t invalidations{0};
};

struct ReadTextSingleflightStats {
  std::uint64_t leaders_started{0};
  std::uint64_t followers_joined{0};
  std::uint64_t bypassed_capacity{0};
  std::uint64_t completions{0};
  std::uint64_t errors{0};
  std::size_t current_in_flight{0};
  std::size_t current_waiters{0};
};

enum class WriteTextDurability {
  rename_only,
  fsync_file,
  fsync_file_and_parent,
};

struct WriteTextOptions {
  WriteMode mode{WriteMode::truncate};
  bool create_parent_directories{false};
  // Commit via a sibling temp file + rename — atomic on POSIX same-filesystem.
  // Only valid when `mode == WriteMode::truncate`.
  bool atomic{false};
  WriteTextDurability durability{WriteTextDurability::rename_only};
};

enum class DirectoryEntryKind { regular_file, directory, symlink, other };

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

async::Awaitable<core::Result<std::string>>
read_text_file(asio::any_io_executor executor, std::string path, ReadTextOptions = {});

async::Awaitable<core::Result<ReadTextResult>>
read_text_file_ranged(asio::any_io_executor executor, std::string path, ReadTextOptions = {});

void invalidate_read_text_file_ranged_cache(std::string_view path);

async::Awaitable<core::Result<ReadTextFileWatchStats>>
watch_read_text_file_ranged_cache(asio::any_io_executor executor,
                                  std::string root,
                                  ReadTextFileWatchOptions = {});

ReadTextFileCacheStats read_text_file_ranged_cache_stats();

ReadTextSingleflightStats read_text_file_ranged_singleflight_stats();

template <typename Fn>
async::Awaitable<std::invoke_result_t<Fn&>>
run_blocking(asio::any_io_executor executor, Fn fn);

async::Awaitable<core::Result<void>>
write_text_file(asio::any_io_executor executor,
                std::string path,
                std::string contents,
                WriteTextOptions = {});

async::Awaitable<core::Result<void>>
write_text_file(asio::any_io_executor executor,
                FileMutation mutation,
                std::string contents,
                WriteTextOptions = {});

async::Awaitable<core::Result<std::vector<DirectoryEntry>>>
list_directory(asio::any_io_executor executor, std::string path, ListDirectoryOptions = {});

async::Awaitable<core::Result<DeletePathResult>>
delete_path(asio::any_io_executor executor, std::string path, DeletePathOptions = {});

async::Awaitable<core::Result<void>>
delete_file(asio::any_io_executor executor, std::string path);

}  // namespace orangutan::io
```

Public paths are UTF-8 `std::string` values. `std::filesystem::path` stays in
`src/oran-io/` so public headers avoid the compile-time cost of `<filesystem>`.

## Cancellation

The slice-2 implementation is cancel-aware at the coroutine boundary:

1. Check cancellation before dispatching onto the supplied executor.
2. Hop to that executor with `asio::post`.
3. Check cancellation again before entering the blocking standard-library file call.

Standard C++ file operations cannot be interrupted mid-call. Callers should use the
runtime's CPU/blocking executor for large or slow local operations once bootstrap
starts threading that service through the system.

## Error Mapping

`oran-io` maps file-system failures into the shared error model:

| Condition | Error kind |
| --- | --- |
| Empty path, invalid limit, non-file read target, non-directory list target, directory delete without `recursive=true`, symlink delete | `invalid_argument` |
| Missing file or directory | `not_found` |
| Existing destination with `WriteMode::fail_if_exists` | `conflict` |
| Permission denied | `permission_denied` |
| Other file-system or stream failure | `io` |

Every returned error includes a `path` context field when a concrete path is known.

## Security Boundary

This library deliberately does **not** evaluate permissions or publish hooks. Tool
execution, skills, and channel attachment handling must wrap `oran-io` calls with:

1. `oran-permission::Evaluator`
2. hook bus pre/post events
3. user-visible audit/log output

That separation keeps reusable local I/O simple while preserving a single policy
surface for effectful agent actions.

## Future Slices

- Globbing with deterministic ordering and max-entry caps.
- Subprocess and pipe helpers backed by asio process support.
- Signal helpers for bootstrap shutdown.
- Watcher APIs if a concrete product flow needs them.
- Permission/hook wrappers in the owning higher-level libraries.
- **Range-aware reads + fingerprints (v2)** — see
  [`../product-specs/0011-file-view-and-caching.md`](../product-specs/0011-file-view-and-caching.md).
  Slice 42 (2026-05-22) shipped the first piece: `io::FileFingerprint`
  (`size_bytes`, `mtime_ns`, reserved `optional<string> sha256`) plus a
  synchronous `io::compute_file_fingerprint(path)` helper. Slice 43
  (2026-05-22) layered the second piece: `io::read_text_file_ranged`
  returning `ReadTextResult { text, fingerprint, start_line, end_line,
  returned_bytes, truncated }`, plus `io::FileRange { LineSpan |
  ByteSpan }` mutual-exclusion validation, dual-end UTF-8 code-point
  boundary alignment for byte ranges, and mid-read change detection
  that retries small whole-file reads once and surfaces `Error::conflict`
  for larger or ranged reads. Slice 45 (2026-05-22) consumes the range
  surface from `oran-tool`'s `FileRead` v2 schema and adds the new
  `Error::not_modified` enum kind that powers the `if_version`
  short-circuit (the opaque token `v1:<sha256(canonical_path)>:<size>:<mtime_ns>`
  is computed at the tool layer; the io layer stays content-only).
  Slice 50 (2026-05-23) adds the first bounded cache consumer in this
  library: large-file line ranges use a `core::BoundedCache`-backed
  line-offset index keyed by canonical path + cheap fingerprint and
  invalidated for the affected canonical path after successful in-process
  writes/deletes. Slice 52 (2026-05-24) adds the bounded file-view cache
  for `read_text_file_ranged`, with metadata validation before hits and
  synchronous invalidation for the affected canonical path after successful
  in-process writes/deletes.
  Slice 57 (2026-05-24) narrows that invalidation to the affected
  canonical path and exposes
  `invalidate_read_text_file_ranged_cache(path)` for watcher
  callbacks. Slice 58 (2026-05-24) adds the Linux/inotify-backed
  watcher callback source in `oran-io` itself:
  `watch_read_text_file_ranged_cache(executor, root, options)` can watch
  one directory or a recursive tree, returns aggregate
  `ReadTextFileWatchStats`, and is cancel-aware for the production
  run-until-cancelled shape. Slice 253 (2026-06-18) wires it as a runtime
  service: `orangutan --serve` auto-starts it over the workspace root.
  Slice 53 (2026-05-24) adds the bounded singleflight table for cold
  `read_text_file_ranged` calls and the
  `ReadTextSingleflightStats` observability snapshot. Slice 54
  (2026-05-24) adds the paired `ReadTextFileCacheStats` snapshot for the
  line-offset index and file-view cache. Future slices wire
  `compute_hash=true` (SHA-256 in
  `FileFingerprint::sha256`) for high-trust paths. Text-only callers keep
  the legacy `read_text_file` wrapper that drops the metadata.
- **Public blocking boundary** — slice 154 exports
  `<oran/io/blocking.hpp>` with `io::run_blocking(executor, fn)`.
  The helper posts a nullary callable returning `core::Result<T>` onto
  the supplied executor, checks cancellation before and after the post,
  and returns `Error::cancelled` without invoking the callable when the
  parent coroutine is already cancelled. The existing file/directory
  helpers consume the public template; future short blocking IO callers
  should reuse it rather than duplicating coroutine-posting glue.

## Atomic Writes

`WriteTextOptions::atomic` selects a temp-then-rename commit path:

1. Compute a sibling temp leaf `.<basename>.orangutan.tmp.<pid>.<random>` under the
   target's parent directory. The leading `.` keeps the temp out of LLM-facing
   directory listings (which hide dotfiles by default). The implementation
   opens the candidate with exclusive creation and retries on the unlikely
   collision, so separate `orangutan` processes cannot overwrite each other's
   temp files.
2. Open the temp with exclusive create, write `contents`, and close it. When
   `WriteTextOptions::durability` is `fsync_file` or
   `fsync_file_and_parent`, fsync the temp file before close.
3. `std::filesystem::rename(temp, target)` - atomic on POSIX when temp and
   target sit on the same filesystem, which is always true because the temp
   lives in the target's parent.
4. When durability is `fsync_file_and_parent`, fsync the target's parent
   directory after the rename so the directory entry is durable. The file-view
   caches are invalidated immediately after a successful rename, before the
   parent fsync, because the target bytes have already changed.
5. Any error before or during rename is followed by a best-effort
   `std::filesystem::remove(temp)` so a failed commit never leaves the
   `.orangutan.tmp` leftover behind.

`mode = append` and `mode = fail_if_exists` are incompatible with this pattern
and reject as `invalid_argument` before any I/O — surfacing the contract
mismatch up-front beats silently overwriting whichever side wins the race.
Non-default durability also rejects unless `atomic=true`; ordinary truncate /
append / fail-if-exists writes keep their existing no-fsync behavior.
