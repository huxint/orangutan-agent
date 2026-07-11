// Shared range-read algorithms over a random-access byte reader.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <oran/core/error.hpp>
#include <oran/core/result.hpp>
#include <oran/io/range.hpp>

namespace orangutan::io::detail::read_text {

constexpr std::size_t kChunkSize = 8192U;

[[nodiscard]] inline std::pair<std::size_t, std::size_t> align_to_utf8_boundaries(std::string_view buffer) noexcept {
  std::size_t head = 0;
  while (head < buffer.size()) {
    const auto byte = static_cast<std::uint8_t>(buffer[head]);
    if ((byte & 0xC0U) != 0x80U) {
      break;
    }
    ++head;
  }

  std::size_t end = head;
  std::size_t cursor = head;
  while (cursor < buffer.size()) {
    const auto byte = static_cast<std::uint8_t>(buffer[cursor]);
    std::size_t length = 0;
    if (byte < 0x80U) {
      length = 1U;
    } else if (byte < 0xC2U) {
      break;
    } else if (byte < 0xE0U) {
      length = 2U;
    } else if (byte < 0xF0U) {
      length = 3U;
    } else if (byte < 0xF5U) {
      length = 4U;
    } else {
      break;
    }
    if (cursor + length > buffer.size()) {
      break;
    }
    cursor += length;
    end = cursor;
  }
  return {head, end};
}

[[nodiscard]] inline core::Result<void> validate_range(const FileRange& range) {
  const bool has_lines = range.lines.has_value();
  const bool has_bytes = range.bytes.has_value();
  if (has_lines == has_bytes) {
    return std::unexpected(core::Error::invalid_argument("FileRange must specify exactly one of lines or bytes"));
  }
  if (has_lines) {
    if (range.lines->start_line == 0 || range.lines->line_count == 0) {
      return std::unexpected(core::Error::invalid_argument("line range fields must be non-zero"));
    }
    return {};
  }
  if (range.bytes->offset_bytes == 0 || range.bytes->length_bytes == 0) {
    return std::unexpected(core::Error::invalid_argument("byte range fields must be non-zero"));
  }
  return {};
}

[[nodiscard]] inline std::uint64_t count_line_span(std::string_view text) noexcept {
  if (text.empty()) {
    return 0;
  }
  const auto newlines = static_cast<std::uint64_t>(std::ranges::count(text, '\n'));
  return newlines + (text.back() == '\n' ? 0U : 1U);
}

template <typename Reader>
[[nodiscard]] core::Result<std::string> read_bytes(Reader& reader, std::uintmax_t offset, std::uintmax_t length) {
  std::string text;
  // Tool input can control `length`; cap eager allocation and let actual
  // bytes read drive any later growth.
  text.reserve(static_cast<std::size_t>(std::min<std::uintmax_t>(length, 64U * 1024U)));
  std::array<char, kChunkSize> buffer{};
  while (static_cast<std::uintmax_t>(text.size()) < length) {
    const auto remaining = length - static_cast<std::uintmax_t>(text.size());
    const auto requested = static_cast<std::size_t>(std::min<std::uintmax_t>(buffer.size(), remaining));
    auto count =
        reader.read(offset + static_cast<std::uintmax_t>(text.size()), std::span<char>{buffer.data(), requested});
    if (!count) {
      return std::unexpected(std::move(count).error());
    }
    if (*count == 0U) {
      break;
    }
    text.append(buffer.data(), *count);
  }
  return text;
}

template <typename Reader>
[[nodiscard]] core::Result<ReadTextResult>
read_whole(Reader& reader, std::uintmax_t file_size, std::uintmax_t max_bytes) {
  const auto requested = std::min(file_size, max_bytes);
  auto text = read_bytes(reader, 0U, requested);
  if (!text) {
    return std::unexpected(std::move(text).error());
  }

  ReadTextResult result;
  result.text = std::move(*text);
  result.truncated = file_size > max_bytes;
  if (result.truncated && !result.text.empty()) {
    const auto [head, end] = align_to_utf8_boundaries(result.text);
    result.text = result.text.substr(head, end - head);
  }
  result.returned_bytes = static_cast<std::uintmax_t>(result.text.size());
  result.start_line = 1;
  result.end_line = count_line_span(result.text);
  return result;
}

template <typename Reader>
[[nodiscard]] core::Result<ReadTextResult>
read_lines(Reader& reader, FileRange::LineSpan lines, std::uintmax_t max_bytes) {
  ReadTextResult result;
  result.start_line = lines.start_line;
  result.end_line = lines.start_line - 1U;

  std::uint64_t current_line = 1;
  std::uint64_t emitted_lines = 0;
  std::uintmax_t offset = 0;
  std::array<char, kChunkSize> buffer{};
  while (emitted_lines < lines.line_count) {
    auto count = reader.read(offset, buffer);
    if (!count) {
      return std::unexpected(std::move(count).error());
    }
    if (*count == 0U) {
      break;
    }
    offset += *count;

    for (std::size_t index = 0; index < *count; ++index) {
      const char ch = buffer[index];
      const bool in_range = current_line >= lines.start_line && emitted_lines < lines.line_count;
      if (in_range) {
        if (static_cast<std::uintmax_t>(result.text.size()) >= max_bytes) {
          result.truncated = true;
          break;
        }
        result.text.push_back(ch);
      }
      if (ch == '\n') {
        if (in_range) {
          ++emitted_lines;
          result.end_line = lines.start_line + emitted_lines - 1U;
        }
        ++current_line;
      }
    }
    if (result.truncated) {
      break;
    }
  }

  if (!result.text.empty() && result.text.back() != '\n' && emitted_lines < lines.line_count) {
    ++emitted_lines;
    result.end_line = lines.start_line + emitted_lines - 1U;
  }
  if (result.truncated && !result.text.empty()) {
    const auto [head, end] = align_to_utf8_boundaries(result.text);
    result.text = result.text.substr(head, end - head);
  }
  result.returned_bytes = static_cast<std::uintmax_t>(result.text.size());
  return result;
}

template <typename Reader>
[[nodiscard]] core::Result<ReadTextResult>
read_bytes_range(Reader& reader, FileRange::ByteSpan bytes, std::uintmax_t max_bytes) {
  const auto requested = std::min(bytes.length_bytes, max_bytes);
  auto text = read_bytes(reader, bytes.offset_bytes, requested);
  if (!text) {
    return std::unexpected(std::move(text).error());
  }

  ReadTextResult result;
  result.text = std::move(*text);
  result.truncated = max_bytes < bytes.length_bytes && result.text.size() >= max_bytes;
  if (!result.text.empty()) {
    const auto [head, end] = align_to_utf8_boundaries(result.text);
    result.text = result.text.substr(head, end - head);
  }
  result.returned_bytes = static_cast<std::uintmax_t>(result.text.size());
  return result;
}

}  // namespace orangutan::io::detail::read_text
