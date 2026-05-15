// src/oran-io/file.cpp — file and directory helper implementation.

#include <oran/io/file.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/cancellation_type.hpp>
#include <asio/post.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <oran/core/error.hpp>

namespace orangutan::io {

namespace {

constexpr std::size_t kReadChunkSize = 8192;

[[nodiscard]] bool is_cancelled(const asio::cancellation_state& cancellation) noexcept {
  return cancellation.cancelled() != asio::cancellation_type::none;
}

[[nodiscard]] bool is_hidden(std::string_view name) noexcept {
  return !name.empty() && name.front() == '.';
}

[[nodiscard]] std::filesystem::path to_path(const std::string& path) {
  return std::filesystem::path{path};
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

[[nodiscard]] core::Result<std::string> read_text_file_blocking(const std::string& path, ReadTextOptions options) {
  if (auto valid = validate_path(path); !valid) {
    return std::unexpected(valid.error());
  }
  if (options.max_bytes == 0) {
    return std::unexpected(core::Error::invalid_argument("max_bytes must be greater than zero").with("path", path));
  }
  if (auto regular = ensure_readable_regular_file(path); !regular) {
    return std::unexpected(regular.error());
  }

  try {
    errno = 0;
    std::ifstream input{to_path(path), std::ios::binary};
    if (!input) {
      return std::unexpected(stream_open_error("failed to open file for reading", path));
    }

    std::string contents;
    std::array<char, kReadChunkSize> buffer{};
    while (input) {
      input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
      const auto count = input.gcount();
      if (count > 0) {
        const auto next_size = static_cast<std::uintmax_t>(contents.size()) + static_cast<std::uintmax_t>(count);
        if (next_size > options.max_bytes) {
          return std::unexpected(core::Error::invalid_argument("file exceeds max_bytes")
                                     .with("path", path)
                                     .with("max_bytes", std::to_string(options.max_bytes)));
        }
        contents.append(buffer.data(), static_cast<std::size_t>(count));
      }
    }

    if (input.bad()) {
      return std::unexpected(io_error("failed while reading file", path));
    }
    return contents;
  } catch (const std::filesystem::filesystem_error& e) {
    return std::unexpected(system_io_error("filesystem read failed", path, e.code()));
  } catch (const std::exception& e) {
    return std::unexpected(io_error("file read failed", path).with("exception", e.what()));
  }
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

[[nodiscard]] core::Result<void>
write_text_file_blocking(const std::string& path, const std::string& contents, WriteTextOptions options) {
  if (auto valid = validate_path(path); !valid) {
    return std::unexpected(valid.error());
  }
  if (static_cast<std::uintmax_t>(contents.size()) >
      static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
    return std::unexpected(core::Error::invalid_argument("contents too large to write").with("path", path));
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

    auto mode = std::ios::binary | std::ios::out;
    if (options.mode == WriteMode::append) {
      mode |= std::ios::app;
    } else if (options.mode == WriteMode::fail_if_exists) {
      mode |= std::ios::trunc | std::ios::noreplace;
    } else {
      mode |= std::ios::trunc;
    }

    errno = 0;
    std::ofstream output{fs_path, mode};
    if (!output) {
      return std::unexpected(stream_open_error("failed to open file for writing", path));
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output) {
      return std::unexpected(io_error("failed while writing file", path));
    }
    return {};
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

}  // namespace

async::Awaitable<core::Result<std::string>>
read_text_file(asio::any_io_executor executor, std::string path, ReadTextOptions options) {
  co_return co_await run_blocking<core::Result<std::string>>(std::move(executor), [path = std::move(path), options] {
    return read_text_file_blocking(path, options);
  });
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

}  // namespace orangutan::io
