// src/oran-io/fingerprint.cpp — size + mtime fingerprint implementation.

#include <oran/io/fingerprint.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include <oran/core/error.hpp>

namespace orangutan::io {
namespace {

using ::orangutan::core::Error;
using ::orangutan::core::Result;

[[nodiscard]] Error
filesystem_error(std::string message, const std::filesystem::path& path, const std::error_code& ec) {
  return Error::io(std::move(message)).with("path", path.string()).with("detail", ec.message());
}

/// Convert `std::filesystem::file_time_type` to nanoseconds since the Unix
/// epoch. The conversion goes via `clock_cast` so the result is independent
/// of whether the filesystem clock equals the system clock on this
/// implementation (libstdc++ ties them on Linux; libc++ does not).
[[nodiscard]] std::uint64_t to_unix_nanoseconds(std::filesystem::file_time_type ftt) noexcept {
  const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(ftt);
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(sys.time_since_epoch()).count();
  return ns < 0 ? std::uint64_t{0} : static_cast<std::uint64_t>(ns);
}

}  // namespace

Result<FileFingerprint> compute_file_fingerprint(std::string_view path) {
  if (path.empty()) {
    return std::unexpected(Error::invalid_argument("fingerprint path must not be empty"));
  }

  const auto fs_path = std::filesystem::path{std::string{path}};
  std::error_code ec;
  const auto status = std::filesystem::status(fs_path, ec);
  if (ec && ec != std::errc::no_such_file_or_directory) {
    return std::unexpected(filesystem_error("failed to inspect file for fingerprint", fs_path, ec));
  }
  if (!std::filesystem::exists(status)) {
    return std::unexpected(Error::not_found("file does not exist").with("path", std::string{path}));
  }
  if (!std::filesystem::is_regular_file(status)) {
    return std::unexpected(Error::io("fingerprint target is not a regular file").with("path", std::string{path}));
  }

  const auto size = std::filesystem::file_size(fs_path, ec);
  if (ec) {
    return std::unexpected(filesystem_error("failed to read file size for fingerprint", fs_path, ec));
  }

  const auto mtime = std::filesystem::last_write_time(fs_path, ec);
  if (ec) {
    return std::unexpected(filesystem_error("failed to read mtime for fingerprint", fs_path, ec));
  }

  return FileFingerprint{
      .size_bytes = size,
      .mtime_ns = to_unix_nanoseconds(mtime),
      .sha256 = std::nullopt,
  };
}

}  // namespace orangutan::io
