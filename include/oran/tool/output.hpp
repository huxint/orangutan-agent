// include/oran/tool/output.hpp — structured tool output envelope.
//
// Tool handlers always return model-facing text, and may optionally attach
// structured JSON bytes, attachment metadata, and usage metrics. `data_json`
// is serialized JSON, not a `nlohmann::json` value: provider adapters own
// protocol-specific parsing/serialization, while this public header stays
// third-party-free and cheap to include.

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace orangutan::tool {

struct ToolUsage {
  std::optional<std::uintmax_t> bytes_read{};
  std::optional<std::uintmax_t> bytes_written{};
  std::optional<std::uint32_t> files_touched{};
  std::optional<std::uint64_t> match_count{};
  std::optional<double> cost_estimate{};
  std::optional<std::chrono::nanoseconds> wall_time{};
  bool truncated{false};
  bool data_dropped{false};

  [[nodiscard]] bool empty() const noexcept {
    return !bytes_read.has_value() && !bytes_written.has_value() && !files_touched.has_value() &&
           !match_count.has_value() && !cost_estimate.has_value() && !wall_time.has_value() && !truncated &&
           !data_dropped;
  }

  friend bool operator==(const ToolUsage&, const ToolUsage&) = default;
};

struct Attachment {
  std::string file_path;
  std::string mime_type;
  std::optional<std::uintmax_t> byte_size{};
  std::optional<std::string> fingerprint{};

  friend bool operator==(const Attachment&, const Attachment&) = default;
};

/// One tool's response. `text` is required and remains the provider fallback
/// for protocols that do not consume structured data yet.
struct Output {
  std::string text;
  std::optional<std::string> data_json{};
  std::vector<Attachment> attachments{};
  ToolUsage usage{};
  bool is_error{false};

  [[nodiscard]] static Output text_only(std::string text) {
    return Output{.text = std::move(text)};
  }

  [[nodiscard]] static Output error(std::string message, std::optional<std::string> data_json = std::nullopt) {
    return Output{.text = std::move(message), .data_json = std::move(data_json), .is_error = true};
  }

  friend bool operator==(const Output&, const Output&) = default;
};

}  // namespace orangutan::tool
