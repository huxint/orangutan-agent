// tests/desktop/test_desktop.cpp — oran-desktop surface coverage.

#include <catch2/catch_test_macros.hpp>

#include <oran/desktop/desktop.hpp>

namespace orangutan::desktop {

TEST_CASE("gui_compiled reflects the build configuration", "[desktop]") {
  // The Slint shell is gated behind `--desktop=y` (ORAN_ENABLE_DESKTOP). The
  // bridge/view-model layer this bucket grows into is always built, so this
  // case runs in every `xmake test`; it asserts the build-config accessor
  // agrees with how the test binary itself was configured.
#if defined(ORAN_ENABLE_DESKTOP)
  REQUIRE(gui_compiled());
#else
  REQUIRE_FALSE(gui_compiled());
#endif
}

}  // namespace orangutan::desktop
