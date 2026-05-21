# IO Runtime

`oran-io` is the policy-free platform library for local file-system and process I/O.
It sits below tools, skills, hooks, channels, and bootstrap: those higher layers decide
whether an action is allowed and which lifecycle event to publish; `oran-io` only
performs the requested operation and returns `core::Result<T>`.

> **Slice-2 status (2026-05-15):** `oran-io` ships text-file read/write helpers and
> deterministic directory listing. Subprocesses, pipes, signals, glob expansion,
> watchers, and permission/hook integration are planned future slices.
>
> **Slice-30 status (2026-05-20):** `oran-io` adds `delete_file(executor, path)`
> for regular-file removal. Directories and symlinks reject as
> `invalid_argument` so the v1 surface cannot be used to recursively
> destroy a tree or unlink a symlink that points outside the workspace.
> The future direction for filesystem mutation is consolidation into a
> single delete helper that handles files AND folders (with recursion
> intent expressed by the caller), not separate per-kind helpers.
>
> **Slice-32 status (2026-05-21):** `WriteTextOptions` grows an opt-in
> `atomic` flag. When set on a `WriteMode::truncate` write, the helper
> stages the contents in a sibling `.<name>.orangutan.tmp.<seq>` and
> commits via `std::filesystem::rename` — atomic on POSIX same-filesystem
> rename(2), so a crash or partial write leaves the original target
> intact instead of truncated. The flag rejects `append` and
> `fail_if_exists` with `invalid_argument` (temp-then-rename has no
> coherent semantics for either). The tool layer wires `file.edit`
> through the atomic path on every rewrite, and `file.write` through
> the atomic path whenever `mode == truncate`; the deep-review BUG-4.1.1
> data-loss footgun is closed.

## Public Surface

```cpp
namespace orangutan::io {

enum class WriteMode { truncate, append, fail_if_exists };

struct ReadTextOptions {
  std::uintmax_t max_bytes{16U * 1024U * 1024U};
};

struct WriteTextOptions {
  WriteMode mode{WriteMode::truncate};
  bool create_parent_directories{false};
  // Commit via a sibling temp file + rename — atomic on POSIX same-filesystem.
  // Only valid when `mode == WriteMode::truncate`.
  bool atomic{false};
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

async::Awaitable<core::Result<void>>
write_text_file(asio::any_io_executor executor,
                std::string path,
                std::string contents,
                WriteTextOptions = {});

async::Awaitable<core::Result<std::vector<DirectoryEntry>>>
list_directory(asio::any_io_executor executor, std::string path, ListDirectoryOptions = {});

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
| Empty path, invalid limit, non-file read target, non-directory list target, `delete_file` on a non-regular file | `invalid_argument` |
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

## Atomic Writes

`WriteTextOptions::atomic` selects a temp-then-rename commit path:

1. Compute a sibling temp leaf `.<basename>.orangutan.tmp.<seq>` under the
   target's parent directory. The leading `.` keeps the temp out of LLM-facing
   directory listings (which hide dotfiles by default); the sequence number is
   drawn from a process-local `std::atomic<uint64_t>` so concurrent writers to
   the same final path never share a temp leaf.
2. Open the temp with `truncate | binary`, write `contents`, explicit flush +
   close.
3. `std::filesystem::rename(temp, target)` — atomic on POSIX when temp and
   target sit on the same filesystem, which is always true because the temp
   lives in the target's parent.
4. Any error on (2) or (3) is followed by a best-effort
   `std::filesystem::remove(temp)` so a failed commit never leaves the
   `.orangutan.tmp` leftover behind.

`mode = append` and `mode = fail_if_exists` are incompatible with this pattern
and reject as `invalid_argument` before any I/O — surfacing the contract
mismatch up-front beats silently overwriting whichever side wins the race.
