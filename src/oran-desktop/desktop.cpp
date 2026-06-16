// src/oran-desktop/desktop.cpp — always-built oran-desktop surface.

#include <oran/desktop/desktop.hpp>

namespace orangutan::desktop {

bool gui_compiled() noexcept {
#if defined(ORAN_ENABLE_DESKTOP)
  return true;
#else
  return false;
#endif
}

}  // namespace orangutan::desktop
