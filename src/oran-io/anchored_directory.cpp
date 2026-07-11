// src/oran-io/anchored_directory.cpp — directory enumeration from authority.

#include <oran/io/directory_authority.hpp>
#include <oran/io/file.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <oran/io/blocking.hpp>

namespace orangutan::io {
namespace {

class UniqueFd {
public:
  explicit UniqueFd(int fd = -1) noexcept : fd_{fd} {}
  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;
  ~UniqueFd() {
    if (fd_ >= 0) {
      static_cast<void>(::close(fd_));
    }
  }
  [[nodiscard]] int get() const noexcept {
    return fd_;
  }
  [[nodiscard]] int release() noexcept {
    return std::exchange(fd_, -1);
  }

private:
  int fd_;
};

class UniqueDir {
public:
  explicit UniqueDir(DIR* directory = nullptr) noexcept : directory_{directory} {}
  UniqueDir(const UniqueDir&) = delete;
  UniqueDir& operator=(const UniqueDir&) = delete;
  ~UniqueDir() {
    if (directory_ != nullptr) {
      static_cast<void>(::closedir(directory_));
    }
  }
  [[nodiscard]] DIR* get() const noexcept {
    return directory_;
  }

private:
  DIR* directory_;
};

[[nodiscard]] core::Error descriptor_error(std::string message, const DirectoryAuthority& directory, int error_number) {
  return core::Error::io(std::move(message))
      .with("path", std::string{directory.display_root()})
      .with("errno", std::to_string(error_number))
      .with("detail", std::generic_category().message(error_number));
}

[[nodiscard]] DirectoryEntryKind classify(mode_t mode) noexcept {
  if (S_ISREG(mode)) {
    return DirectoryEntryKind::regular_file;
  }
  if (S_ISDIR(mode)) {
    return DirectoryEntryKind::directory;
  }
  if (S_ISLNK(mode)) {
    return DirectoryEntryKind::symlink;
  }
  return DirectoryEntryKind::other;
}

[[nodiscard]] core::Result<std::vector<DirectoryEntry>>
list_authorized_directory_blocking(const DirectoryAuthority& directory, ListDirectoryOptions options) {
  if (directory.native_handle() < 0) {
    return std::unexpected(core::Error::internal("directory authority is empty"));
  }
  if (options.max_entries == 0U) {
    return std::unexpected(core::Error::invalid_argument("max_entries must be greater than zero")
                               .with("path", std::string{directory.display_root()}));
  }

  auto readable = UniqueFd{::openat(directory.native_handle(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
  if (readable.get() < 0) {
    return std::unexpected(descriptor_error("failed to open authorized directory", directory, errno));
  }
  auto* raw_stream = ::fdopendir(readable.get());
  if (raw_stream == nullptr) {
    return std::unexpected(descriptor_error("failed to enumerate authorized directory", directory, errno));
  }
  static_cast<void>(readable.release());
  auto stream = UniqueDir{raw_stream};

  std::vector<DirectoryEntry> entries;
  while (true) {
    errno = 0;
    const auto* entry = ::readdir(stream.get());
    if (entry == nullptr) {
      if (errno != 0) {
        return std::unexpected(descriptor_error("failed while enumerating authorized directory", directory, errno));
      }
      break;
    }
    const auto name = std::string_view{entry->d_name};
    if (name == "." || name == ".." || (!options.include_hidden && name.starts_with('.'))) {
      continue;
    }
    if (entries.size() >= options.max_entries) {
      return std::unexpected(core::Error::io("directory entry limit exceeded")
                                 .with("path", std::string{directory.display_root()})
                                 .with("max_entries", std::to_string(options.max_entries)));
    }

    struct stat status{};
    if (::fstatat(directory.native_handle(), entry->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0) {
      return std::unexpected(descriptor_error("failed to inspect authorized directory entry", directory, errno));
    }
    const auto kind = classify(status.st_mode);
    entries.push_back(DirectoryEntry{
        .name = std::string{name},
        .path = std::format("{}/{}", directory.display_root(), name),
        .kind = kind,
        .size_bytes = kind == DirectoryEntryKind::regular_file
                          ? std::optional<std::uintmax_t>{static_cast<std::uintmax_t>(status.st_size)}
                          : std::nullopt,
    });
  }

  std::ranges::sort(entries, {}, &DirectoryEntry::path);
  return entries;
}

}  // namespace

async::Awaitable<core::Result<std::vector<DirectoryEntry>>>
list_directory(asio::any_io_executor executor, DirectoryAuthority directory, ListDirectoryOptions options) {
  co_return co_await run_blocking(std::move(executor), [directory = std::move(directory), options]() mutable {
    try {
      return list_authorized_directory_blocking(directory, options);
    } catch (const std::exception& error) {
      return core::Result<std::vector<DirectoryEntry>>{
          std::unexpected(core::Error::io("authorized directory listing failed").with("detail", error.what()))};
    } catch (...) {
      return core::Result<std::vector<DirectoryEntry>>{
          std::unexpected(core::Error::io("authorized directory listing failed"))};
    }
  });
}

}  // namespace orangutan::io
