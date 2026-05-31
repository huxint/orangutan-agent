// src/oran-memory/framing.cpp — once-per-turn prompt memory framing owner.

#include <oran/memory/framing.hpp>

#include <utility>

namespace orangutan::memory {

FramingOwner::FramingOwner(Framing framing) : framing_{std::move(framing)} {}

std::string_view FramingOwner::render_once() {
  ++stats_.renders;
  return framing_.section_text;
}

const Framing& FramingOwner::framing() const noexcept {
  return framing_;
}

FramingStats FramingOwner::stats() const noexcept {
  return stats_;
}

void FramingOwner::replace(Framing framing) {
  framing_ = std::move(framing);
}

void FramingOwner::clear() {
  framing_.section_text.clear();
}

}  // namespace orangutan::memory
