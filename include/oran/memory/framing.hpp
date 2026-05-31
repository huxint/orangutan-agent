// include/oran/memory/framing.hpp — once-per-turn prompt memory framing.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace orangutan::memory {

struct Framing {
  std::string section_text;

  friend bool operator==(const Framing&, const Framing&) = default;
};

struct FramingStats {
  std::uint64_t renders{0};

  friend bool operator==(const FramingStats&, const FramingStats&) = default;
};

/// Owner for section-5 prompt memory bytes.
///
/// The current implementation owns already-materialized section text. Keeping it
/// behind this owner gives future long-term recall a single pre-loop boundary to
/// populate without moving memory reads into `agent::Loop` iterations.
class FramingOwner {
public:
  explicit FramingOwner(Framing framing = {});

  [[nodiscard]] std::string_view render_once();
  [[nodiscard]] const Framing& framing() const noexcept;
  [[nodiscard]] FramingStats stats() const noexcept;
  void replace(Framing framing);
  void clear();

private:
  Framing framing_{};
  FramingStats stats_{};
};

}  // namespace orangutan::memory
