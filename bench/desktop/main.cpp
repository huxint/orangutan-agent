// bench/desktop/main.cpp — registers and runs oran-desktop nanobench scenarios.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <oran/desktop/desktop.hpp>

int main() {
  ankerl::nanobench::Bench b;
  b.title("bench-desktop");

  // Placeholder scenario until the bridge/view-model layer lands (Slice C),
  // which is where the meaningful A-vs-B microbench (delta marshalling through
  // the bounded UI<->runtime queue) belongs. See bench/desktop/README.md.
  b.run("gui_compiled", [] { ankerl::nanobench::doNotOptimizeAway(orangutan::desktop::gui_compiled()); });

  return 0;
}
