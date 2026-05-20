// include/oran/io/file.hpp — coroutine file and directory helpers.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <asio/any_io_executor.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>

namespace orangutan::io {

enum class WriteMode : std::uint8_t {
  truncate,
  append,
  fail_if_exists,
};

struct ReadTextOptions {
  std::uintmax_t max_bytes{16U * 1024U * 1024U};
};

struct WriteTextOptions {
  WriteMode mode{WriteMode::truncate};
  bool create_parent_directories{false};
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
