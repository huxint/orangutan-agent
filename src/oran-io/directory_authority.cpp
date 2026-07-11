// src/oran-io/directory_authority.cpp — Linux-first dirfd path resolution.

#include <oran/io/directory_authority.hpp>

#include <cerrno>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/openat2.h>
#include <sys/syscall.h>
#endif

#include <oran/core/error.hpp>

namespace orangutan::io {
namespace {

constexpr std::size_t kResolutionRaceRetries = 3U;

class UniqueFd {
public:
  explicit UniqueFd(int fd = -1) noexcept : fd_{fd} {}

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  UniqueFd(UniqueFd&& other) noexcept : fd_{other.release()} {}

  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  ~UniqueFd() {
    reset();
  }

  [[nodiscard]] int get() const noexcept {
    return fd_;
  }

  [[nodiscard]] int release() noexcept {
    const auto fd = fd_;
    fd_ = -1;
    return fd;
  }

  void reset(int next = -1) noexcept {
    if (fd_ >= 0) {
      static_cast<void>(::close(fd_));
    }
    fd_ = next;
  }

private:
  int fd_{-1};
};

[[nodiscard]] core::Error
path_error(std::string message, std::string_view root, std::string_view relative_path, std::string_view reason) {
  return core::Error::permission_denied(std::move(message))
      .with("root", std::string{root})
      .with("path", std::string{relative_path})
      .with("reason", std::string{reason});
}

[[nodiscard]] core::Error
errno_error(std::string message, std::string_view root, std::string_view relative_path, int error_number) {
  auto with_context = [&](core::Error error) {
    return std::move(error)
        .with("root", std::string{root})
        .with("path", std::string{relative_path})
        .with("errno", std::to_string(error_number))
        .with("detail", std::generic_category().message(error_number));
  };

  switch (error_number) {
    case ENOENT:
      return with_context(core::Error::not_found(std::move(message)));
    case EACCES:
    case EPERM:
      return with_context(core::Error::permission_denied(std::move(message)));
    case ELOOP:
      return path_error(std::move(message), root, relative_path, "symlink_component");
    case EXDEV:
      return path_error(std::move(message), root, relative_path, "outside_authority");
    case EAGAIN:
      return core::Error{core::ErrorKind::conflict, std::move(message)}
          .with("root", std::string{root})
          .with("path", std::string{relative_path})
          .with("reason", "path_resolution_raced");
    case ENAMETOOLONG:
    case ENOTDIR:
      return with_context(core::Error::invalid_argument(std::move(message)));
    default:
      return with_context(core::Error::io(std::move(message)));
  }
}

[[nodiscard]] core::Result<void> validate_anchored_path(const AnchoredPath& path) {
  if (path.relative_path.empty()) {
    return std::unexpected(core::Error::invalid_argument("anchored path must not be empty"));
  }
  if (path.relative_path.contains('\0')) {
    return std::unexpected(core::Error::invalid_argument("anchored path must not contain NUL"));
  }

  const auto parsed = std::filesystem::path{path.relative_path};
  if (parsed.is_absolute()) {
    return std::unexpected(
        core::Error::invalid_argument("anchored path must be relative").with("path", path.relative_path));
  }
  for (const auto& component : parsed) {
    if (component == "..") {
      return std::unexpected(
          path_error("anchored path must not contain parent traversal", {}, path.relative_path, "outside_authority"));
    }
  }
  return {};
}

[[nodiscard]] std::vector<std::string> path_components(std::string_view relative_path) {
  std::vector<std::string> components;
  for (const auto& component : std::filesystem::path{relative_path}) {
    if (component.empty() || component == ".") {
      continue;
    }
    components.push_back(component.string());
  }
  return components;
}

[[nodiscard]] int duplicate_fd(int fd) noexcept {
#if defined(F_DUPFD_CLOEXEC)
  return ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
#else
  const auto duplicated = ::dup(fd);
  if (duplicated >= 0) {
    static_cast<void>(::fcntl(duplicated, F_SETFD, FD_CLOEXEC));
  }
  return duplicated;
#endif
}

[[nodiscard]] core::Result<UniqueFd>
openat_fallback(int root_fd, std::string_view root, const AnchoredPath& path, int final_flags) {
  auto components = path_components(path.relative_path);
  auto current = UniqueFd{duplicate_fd(root_fd)};
  if (current.get() < 0) {
    return std::unexpected(errno_error("failed to duplicate directory authority", root, path.relative_path, errno));
  }

  if (components.empty()) {
    errno = 0;
    const auto opened = ::openat(current.get(), ".", final_flags | O_NOFOLLOW);
    if (opened < 0) {
      return std::unexpected(errno_error("failed to open anchored path", root, path.relative_path, errno));
    }
    return UniqueFd{opened};
  }

  for (std::size_t index = 0; index + 1U < components.size(); ++index) {
    auto flags = O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    errno = 0;
    auto next = UniqueFd{::openat(current.get(), components[index].c_str(), flags)};
    if (next.get() < 0) {
      return std::unexpected(errno_error("failed to resolve anchored directory", root, path.relative_path, errno));
    }
    current = std::move(next);
  }

  errno = 0;
  const auto opened = ::openat(current.get(), components.back().c_str(), final_flags | O_NOFOLLOW);
  if (opened < 0) {
    return std::unexpected(errno_error("failed to open anchored path", root, path.relative_path, errno));
  }
  return UniqueFd{opened};
}

#if defined(__linux__) && defined(SYS_openat2)
[[nodiscard]] core::Result<UniqueFd>
openat2_beneath(int root_fd, std::string_view root, const AnchoredPath& path, int final_flags) {
  auto how = open_how{};
  how.flags = static_cast<__u64>(final_flags);
  how.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS;
  if (path.symlink_policy == AnchoredSymlinkPolicy::reject_all) {
    how.resolve |= RESOLVE_NO_SYMLINKS;
  }

  for (std::size_t attempt = 0; attempt < kResolutionRaceRetries; ++attempt) {
    errno = 0;
    const auto opened =
        static_cast<int>(::syscall(SYS_openat2, root_fd, path.relative_path.c_str(), &how, sizeof(how)));
    if (opened >= 0) {
      return UniqueFd{opened};
    }
    if (errno == ENOSYS) {
      return openat_fallback(root_fd, root, path, final_flags);
    }
    if (errno != EAGAIN || attempt + 1U == kResolutionRaceRetries) {
      return std::unexpected(errno_error("failed to open anchored path", root, path.relative_path, errno));
    }
  }

  return std::unexpected(core::Error::internal("unreachable anchored path retry state"));
}
#endif

[[nodiscard]] core::Result<UniqueFd>
open_beneath(int root_fd, std::string_view root, const AnchoredPath& path, int final_flags) {
  if (auto valid = validate_anchored_path(path); !valid) {
    return std::unexpected(std::move(valid).error());
  }

#if defined(__linux__) && defined(SYS_openat2)
  return openat2_beneath(root_fd, root, path, final_flags);
#else
  return openat_fallback(root_fd, root, path, final_flags);
#endif
}

[[nodiscard]] core::Result<void> require_regular_file(int fd, std::string_view root, std::string_view relative_path) {
  struct stat status{};
  errno = 0;
  if (::fstat(fd, &status) != 0) {
    return std::unexpected(errno_error("failed to inspect anchored file", root, relative_path, errno));
  }
  if (!S_ISREG(status.st_mode)) {
    return std::unexpected(core::Error::invalid_argument("anchored path is not a regular file")
                               .with("root", std::string{root})
                               .with("path", std::string{relative_path}));
  }
  return {};
}

}  // namespace

struct ReadOnlyFile::Impl {
  Impl(UniqueFd opened_fd, std::string opened_path) : fd{std::move(opened_fd)}, display_path{std::move(opened_path)} {}

  UniqueFd fd;
  std::string display_path;
};

struct DirectoryAuthority::Impl {
  Impl(UniqueFd opened_fd, std::string root) : fd{std::move(opened_fd)}, display_root{std::move(root)} {}

  UniqueFd fd;
  std::string display_root;
};

ReadOnlyFile::ReadOnlyFile(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}

ReadOnlyFile::ReadOnlyFile(ReadOnlyFile&&) noexcept = default;

ReadOnlyFile& ReadOnlyFile::operator=(ReadOnlyFile&&) noexcept = default;

ReadOnlyFile::~ReadOnlyFile() = default;

int ReadOnlyFile::native_handle() const noexcept {
  return impl_ == nullptr ? -1 : impl_->fd.get();
}

std::string_view ReadOnlyFile::display_path() const noexcept {
  return impl_ == nullptr ? std::string_view{} : std::string_view{impl_->display_path};
}

DirectoryAuthority::DirectoryAuthority(std::shared_ptr<const Impl> impl) : impl_{std::move(impl)} {}

core::Result<DirectoryAuthority> DirectoryAuthority::open_trusted(std::string_view root) {
  if (root.empty()) {
    return std::unexpected(core::Error::invalid_argument("directory authority root must not be empty"));
  }
  if (root.contains('\0')) {
    return std::unexpected(core::Error::invalid_argument("directory authority root must not contain NUL"));
  }

  const auto root_string = std::string{root};
  errno = 0;
  auto fd = UniqueFd{::open(root_string.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC)};
  if (fd.get() < 0) {
    return std::unexpected(errno_error("failed to open directory authority", root, ".", errno));
  }

  return DirectoryAuthority{std::make_shared<Impl>(std::move(fd), root_string)};
}

core::Result<ReadOnlyFile> DirectoryAuthority::open_file(const AnchoredPath& path) const {
  if (impl_ == nullptr) {
    return std::unexpected(core::Error::internal("directory authority is empty"));
  }

  auto opened = open_beneath(impl_->fd.get(), impl_->display_root, path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
  if (!opened) {
    return std::unexpected(std::move(opened).error());
  }
  if (auto regular = require_regular_file(opened->get(), impl_->display_root, path.relative_path); !regular) {
    return std::unexpected(std::move(regular).error());
  }
  return ReadOnlyFile{
      std::make_unique<ReadOnlyFile::Impl>(std::move(*opened),
                                           std::format("{}/{}", impl_->display_root, path.relative_path))};
}

core::Result<DirectoryAuthority> DirectoryAuthority::open_directory(const AnchoredPath& path) const {
  if (impl_ == nullptr) {
    return std::unexpected(core::Error::internal("directory authority is empty"));
  }

  auto opened = open_beneath(impl_->fd.get(), impl_->display_root, path, O_PATH | O_DIRECTORY | O_CLOEXEC);
  if (!opened) {
    return std::unexpected(std::move(opened).error());
  }

  auto display = std::format("{}/{}", impl_->display_root, path.relative_path);
  return DirectoryAuthority{std::make_shared<Impl>(std::move(*opened), std::move(display))};
}

std::string_view DirectoryAuthority::display_root() const noexcept {
  return impl_ == nullptr ? std::string_view{} : std::string_view{impl_->display_root};
}

core::Result<bool> DirectoryAuthority::refers_to_path(std::string_view path) const {
  if (impl_ == nullptr) {
    return std::unexpected(core::Error::internal("directory authority is empty"));
  }
  if (path.empty() || path.contains('\0')) {
    return std::unexpected(core::Error::invalid_argument("directory comparison path is invalid"));
  }

  struct stat authority_status{};
  if (::fstat(impl_->fd.get(), &authority_status) != 0) {
    return std::unexpected(errno_error("failed to inspect directory authority", impl_->display_root, ".", errno));
  }

  struct stat path_status{};
  const auto path_string = std::string{path};
  if (::stat(path_string.c_str(), &path_status) != 0) {
    if (errno == ENOENT || errno == ENOTDIR) {
      return false;
    }
    return std::unexpected(errno_error("failed to inspect directory comparison path", path, ".", errno));
  }
  return authority_status.st_dev == path_status.st_dev && authority_status.st_ino == path_status.st_ino;
}

}  // namespace orangutan::io
