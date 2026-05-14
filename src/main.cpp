// src/main.cpp — `orangutan` binary entry point.
//
// The early binary demonstrates the toolchain end-to-end: C++26 build,
// std::print output, Result<T> threading from `bootstrap()` up to the OS exit
// code. The real bootstrap (config load, runtime spawn, signal handling, CLI
// dispatch) lands when oran-bootstrap is implemented.

#include <print>
#include <string>
#include <string_view>

#include <oran/core/error.hpp>
#include <oran/core/result.hpp>

namespace {

using ::orangutan::core::Error;
using ::orangutan::core::Result;

constexpr std::string_view kVersion = "2.0.0-slice1";

[[nodiscard]] Result<int> bootstrap() {
  std::println("orangutan v{}", kVersion);
  std::println("core and async runtime foundations are assembled; agent loop is not implemented yet.");
  std::println("see docs/exec-plans/active/ for what lands next.");
  return 0;
}

}  // namespace

int main() {
  auto r = bootstrap();
  if (!r) {
    std::println(stderr, "orangutan: {}", r.error());
    return 1;
  }
  return *r;
}
