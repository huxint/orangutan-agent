// include/oran/io/directory_authority.hpp — dirfd-backed path authority.

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <oran/core/result.hpp>

namespace orangutan::io {

/// Symlink policy applied while resolving every component beneath a directory
/// authority. The permissive mode still forbids escapes from the authority;
/// the stricter component-wise fallback may reject safe symlinks as well.
enum class AnchoredSymlinkPolicy {
  allow_beneath,
  reject_all,
};

/// An untrusted relative name paired with its resolution policy. The owning
/// DirectoryAuthority supplies the root; absolute paths and `..` components
/// are rejected before any filesystem operation.
struct AnchoredPath {
  std::string relative_path;
  AnchoredSymlinkPolicy symlink_policy{AnchoredSymlinkPolicy::reject_all};
};

/// Move-only readable regular-file handle. The returned native handle is
/// borrowed and remains valid until this object is destroyed or moved from.
class ReadOnlyFile {
public:
  ReadOnlyFile(ReadOnlyFile&&) noexcept;
  ReadOnlyFile& operator=(ReadOnlyFile&&) noexcept;
  ReadOnlyFile(const ReadOnlyFile&) = delete;
  ReadOnlyFile& operator=(const ReadOnlyFile&) = delete;
  ~ReadOnlyFile();

  [[nodiscard]] int native_handle() const noexcept;
  [[nodiscard]] std::string_view display_path() const noexcept;

private:
  struct Impl;

  explicit ReadOnlyFile(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;

  friend class DirectoryAuthority;
};

/// A stable capability for one trusted directory. Operations are resolved
/// relative to an owned directory descriptor, so replacing or renaming the
/// original pathname does not redirect later access.
class DirectoryAuthority {
public:
  /// Open a trusted root directory and retain its identity across renames.
  [[nodiscard]] static core::Result<DirectoryAuthority> open_trusted(std::string_view root);

  DirectoryAuthority(const DirectoryAuthority&) noexcept = default;
  DirectoryAuthority& operator=(const DirectoryAuthority&) noexcept = default;
  DirectoryAuthority(DirectoryAuthority&&) noexcept = default;
  DirectoryAuthority& operator=(DirectoryAuthority&&) noexcept = default;
  ~DirectoryAuthority() = default;

  /// Open a regular file beneath this authority for reading.
  [[nodiscard]] core::Result<ReadOnlyFile> open_file(const AnchoredPath& path) const;

  /// Open a child directory as another stable authority.
  [[nodiscard]] core::Result<DirectoryAuthority> open_directory(const AnchoredPath& path) const;

  /// Original trusted root spelling, retained for diagnostics only.
  [[nodiscard]] std::string_view display_root() const noexcept;

  /// True when `path` still names the directory held by this authority.
  /// A missing/replaced pathname returns false; descriptor errors propagate.
  [[nodiscard]] core::Result<bool> refers_to_path(std::string_view path) const;

private:
  struct Impl;

  explicit DirectoryAuthority(std::shared_ptr<const Impl> impl);

  std::shared_ptr<const Impl> impl_;
};

}  // namespace orangutan::io
