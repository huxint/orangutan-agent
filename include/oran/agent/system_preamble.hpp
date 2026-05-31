// include/oran/agent/system_preamble.hpp - stable system-preamble owner.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace orangutan::agent {

struct SystemPreamble {
  std::string section_text;

  friend bool operator==(const SystemPreamble&, const SystemPreamble&) = default;
};

struct SystemPreambleStats {
  std::uint64_t renders{0};

  friend bool operator==(const SystemPreambleStats&, const SystemPreambleStats&) = default;
};

[[nodiscard]] SystemPreamble default_system_preamble();

/// Owner for section-1 prompt bytes.
///
/// The text is stable by construction: it is rendered from repository-versioned
/// constants and caller-supplied replacement text only. Dynamic sections stay in
/// their own prompt-builder inputs.
class SystemPreambleOwner {
public:
  SystemPreambleOwner();
  explicit SystemPreambleOwner(SystemPreamble preamble);

  [[nodiscard]] std::string_view render_once();
  [[nodiscard]] const SystemPreamble& preamble() const noexcept;
  [[nodiscard]] SystemPreambleStats stats() const noexcept;
  void replace(SystemPreamble preamble);
  void clear();

private:
  SystemPreamble preamble_;
  SystemPreambleStats stats_{};
};

}  // namespace orangutan::agent
