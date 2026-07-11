// src/oran-io/anchored_file.cpp — text reads from authorized file handles.

#include <oran/io/file.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <system_error>
#include <utility>

#include <sys/types.h>
#include <unistd.h>

#include <oran/core/error.hpp>
#include <oran/io/blocking.hpp>
#include <oran/io/directory_authority.hpp>
#include <oran/io/fingerprint.hpp>

#include "_impl/read_text_core.hpp"

namespace orangutan::io {
namespace {

constexpr std::uintmax_t kMidReadRetryThresholdBytes = 64U * 1024U;

[[nodiscard]] core::Error descriptor_error(std::string message, const ReadOnlyFile& file, int error_number) {
  return core::Error::io(std::move(message))
      .with("path", std::string{file.display_path()})
      .with("errno", std::to_string(error_number))
      .with("detail", std::generic_category().message(error_number));
}

class DescriptorReader {
public:
  explicit DescriptorReader(const ReadOnlyFile& file) : file_{file} {}

  [[nodiscard]] core::Result<std::size_t> read(std::uintmax_t offset, std::span<char> destination) const {
    if (offset > static_cast<std::uintmax_t>(std::numeric_limits<off_t>::max())) {
      return std::unexpected(core::Error::invalid_argument("file read offset is too large")
                                 .with("path", std::string{file_.display_path()}));
    }

    while (true) {
      errno = 0;
      const auto count =
          ::pread(file_.native_handle(), destination.data(), destination.size(), static_cast<off_t>(offset));
      if (count >= 0) {
        return static_cast<std::size_t>(count);
      }
      if (errno != EINTR) {
        return std::unexpected(descriptor_error("failed while reading authorized file", file_, errno));
      }
    }
  }

private:
  const ReadOnlyFile& file_;
};

[[nodiscard]] core::Result<ReadTextResult>
dispatch_read(DescriptorReader& reader, const ReadTextOptions& options, const FileFingerprint& fingerprint) {
  if (!options.range) {
    return detail::read_text::read_whole(reader, fingerprint.size_bytes, options.max_bytes);
  }
  if (options.range->lines) {
    return detail::read_text::read_lines(reader, *options.range->lines, options.max_bytes);
  }
  return detail::read_text::read_bytes_range(reader, *options.range->bytes, options.max_bytes);
}

[[nodiscard]] core::Result<ReadTextResult> read_authorized_file_blocking_impl(ReadOnlyFile& file,
                                                                              const ReadTextOptions& options) {
  if (options.max_bytes == 0U) {
    return std::unexpected(core::Error::invalid_argument("max_bytes must be greater than zero")
                               .with("path", std::string{file.display_path()}));
  }
  if (options.range) {
    if (auto valid = detail::read_text::validate_range(*options.range); !valid) {
      return std::unexpected(std::move(valid).error().with("path", std::string{file.display_path()}));
    }
  }

  auto pre = compute_file_fingerprint(file);
  if (!pre) {
    return std::unexpected(std::move(pre).error());
  }

  auto reader = DescriptorReader{file};
  auto result = dispatch_read(reader, options, *pre);
  if (!result) {
    return std::unexpected(std::move(result).error());
  }
  auto post = compute_file_fingerprint(file);
  if (!post) {
    return std::unexpected(std::move(post).error());
  }

  if (*pre != *post) {
    const bool ranged = options.range.has_value();
    const bool large = pre->size_bytes >= kMidReadRetryThresholdBytes;
    if (ranged || large) {
      return std::unexpected(core::Error{core::ErrorKind::conflict, "file changed during read"}
                                 .with("path", std::string{file.display_path()})
                                 .with("size_before", std::to_string(pre->size_bytes))
                                 .with("size_after", std::to_string(post->size_bytes)));
    }

    pre = post;
    result = dispatch_read(reader, options, *pre);
    if (!result) {
      return std::unexpected(std::move(result).error());
    }
    post = compute_file_fingerprint(file);
    if (!post) {
      return std::unexpected(std::move(post).error());
    }
    if (*pre != *post) {
      return std::unexpected(core::Error{core::ErrorKind::conflict, "file changed during read (after retry)"}.with(
          "path",
          std::string{file.display_path()}));
    }
  }

  result->fingerprint = *post;
  return result;
}

[[nodiscard]] core::Result<ReadTextResult> read_authorized_file_blocking(ReadOnlyFile& file,
                                                                         const ReadTextOptions& options) {
  try {
    return read_authorized_file_blocking_impl(file, options);
  } catch (const std::exception& error) {
    return std::unexpected(core::Error::io("authorized file read failed")
                               .with("path", std::string{file.display_path()})
                               .with("exception", error.what()));
  }
}

}  // namespace

async::Awaitable<core::Result<ReadTextResult>>
read_text_file_ranged(asio::any_io_executor executor, ReadOnlyFile file, ReadTextOptions options) {
  co_return co_await run_blocking(std::move(executor), [file = std::move(file), options]() mutable {
    return read_authorized_file_blocking(file, options);
  });
}

}  // namespace orangutan::io
