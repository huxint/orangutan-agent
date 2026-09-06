#pragma once

#include <string>

namespace orangutan::memory {

struct Framing {
  std::string section_text;

  friend bool operator==(const Framing&, const Framing&) = default;
};

}  // namespace orangutan::memory
