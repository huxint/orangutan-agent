// src/oran-io/anchored_directory.cpp — directory enumeration from authority.

#include <oran/io/directory_authority.hpp>
#include <oran/io/file.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
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

namespace {

/// One directory level read relative to a pinned authority. Names are collected
/// with their no-follow kind/size so the walk can classify symlinks without
/// following them and hand directory entries to the visitor before descending.
struct AnchoredLevelEntry {
  std::string name;
  DirectoryEntryKind kind{DirectoryEntryKind::other};
  std::optional<std::uintmax_t> size_bytes;
};

[[nodiscard]] core::Result<std::vector<AnchoredLevelEntry>> read_level(const DirectoryAuthority& directory) {
  if (directory.native_handle() < 0) {
    return std::unexpected(core::Error::internal("directory authority is empty"));
  }

  auto readable = UniqueFd{::openat(directory.native_handle(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
  if (readable.get() < 0) {
    // EACCES keeps its own error kind so `WalkTreeOptions::skip_permission_denied`
    // can distinguish an unreadable subtree from an I/O fault.
    const int error_number = errno;
    if (error_number == EACCES) {
      return std::unexpected(core::Error::permission_denied("failed to open authorized directory")
                                 .with("path", std::string{directory.display_root()})
                                 .with("errno", std::to_string(error_number))
                                 .with("detail", std::generic_category().message(error_number)));
    }
    return std::unexpected(descriptor_error("failed to open authorized directory", directory, error_number));
  }
  auto* raw_stream = ::fdopendir(readable.get());
  if (raw_stream == nullptr) {
    return std::unexpected(descriptor_error("failed to enumerate authorized directory", directory, errno));
  }
  static_cast<void>(readable.release());
  auto stream = UniqueDir{raw_stream};

  std::vector<AnchoredLevelEntry> entries;
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
    if (name == "." || name == "..") {
      continue;
    }

    struct stat status{};
    if (::fstatat(directory.native_handle(), entry->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0) {
      return std::unexpected(descriptor_error("failed to inspect authorized directory entry", directory, errno));
    }
    const auto kind = classify(status.st_mode);
    entries.push_back(AnchoredLevelEntry{
        .name = std::string{name},
        .kind = kind,
        .size_bytes = kind == DirectoryEntryKind::regular_file
                          ? std::optional<std::uintmax_t>{static_cast<std::uintmax_t>(status.st_size)}
                          : std::nullopt,
    });
  }

  std::ranges::sort(entries, {}, &AnchoredLevelEntry::name);
  return entries;
}

struct WalkState {
  WalkTreeOptions options;
  const std::function<bool()>& cancelled;
  WalkVisitor& visitor;
  std::size_t visited{0};
};

[[nodiscard]] core::Result<WalkAction>
walk_level(WalkState& state, const DirectoryAuthority& directory, std::string_view prefix, std::size_t depth) {
  auto level = read_level(directory);
  if (!level) {
    // `depth > 1` keeps the root strict: only subtree enumeration refusals
    // are prunable, and only when the caller opted into the legacy
    // `skip_permission_denied` posture.
    if (depth > 1 && state.options.skip_permission_denied &&
        level.error().kind() == core::ErrorKind::permission_denied) {
      return WalkAction::proceed;
    }
    return std::unexpected(std::move(level).error());
  }

  for (auto& child : *level) {
    if (state.cancelled()) {
      return std::unexpected(core::Error::cancelled());
    }
    if (state.options.max_entries != 0 && state.visited >= state.options.max_entries) {
      return std::unexpected(core::Error::io("directory walk entry limit exceeded")
                                 .with("reason", "walk_entry_limit")
                                 .with("max_entries", std::to_string(state.options.max_entries)));
    }
    ++state.visited;

    auto relative = prefix.empty() ? child.name : std::format("{}/{}", prefix, child.name);
    auto entry = WalkEntry{
        .name = std::move(child.name),
        .relative_path = std::move(relative),
        .kind = child.kind,
        .size_bytes = child.size_bytes,
        .depth = depth,
    };

    auto decision = state.visitor(directory, entry);
    if (!decision) {
      return std::unexpected(std::move(decision).error());
    }
    if (*decision == WalkAction::stop) {
      return WalkAction::stop;
    }
    if (*decision == WalkAction::skip_subtree || entry.kind != DirectoryEntryKind::directory) {
      continue;
    }

    // Descend only through the pinned no-follow child authority. A symlink was
    // already classified above and never reaches this branch, so the walk
    // cannot be redirected outside the root mid-traversal.
    auto subdirectory = directory.open_directory(AnchoredPath{
        .relative_path = entry.name,
        .symlink_policy = AnchoredSymlinkPolicy::reject_all,
    });
    if (!subdirectory) {
      return std::unexpected(std::move(subdirectory).error());
    }
    auto nested = walk_level(state, *subdirectory, entry.relative_path, depth + 1);
    if (!nested) {
      return std::unexpected(std::move(nested).error());
    }
    if (*nested == WalkAction::stop) {
      return WalkAction::stop;
    }
  }
  return WalkAction::proceed;
}

}  // namespace

core::Result<void> walk_directory_tree(const DirectoryAuthority& root,
                                       WalkTreeOptions options,
                                       const std::function<bool()>& cancelled,
                                       WalkVisitor& visitor) {
  if (root.native_handle() < 0) {
    return std::unexpected(core::Error::internal("directory authority is empty"));
  }
  if (!visitor) {
    return std::unexpected(core::Error::invalid_argument("directory walk visitor must not be empty"));
  }
  if (!cancelled) {
    return std::unexpected(core::Error::invalid_argument("directory walk cancel predicate must not be empty"));
  }

  auto state = WalkState{.options = options, .cancelled = cancelled, .visitor = visitor};
  auto walked = walk_level(state, root, {}, 1);
  if (!walked) {
    return std::unexpected(std::move(walked).error());
  }
  return {};
}

}  // namespace orangutan::io
