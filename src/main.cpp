// src/main.cpp — `orangutan` binary entry point.
//
// The binary delegates runtime setup to oran-bootstrap and only translates the
// expected error boundary into a process exit code.

#include <print>
#include <span>
#include <string_view>
#include <vector>

#include <oran/bootstrap.hpp>
#include <oran/core/error.hpp>

namespace {

std::vector<std::string_view> args_from(int argc, char** argv) {
  auto args = std::vector<std::string_view>{};
  if (argc <= 1) {
    return args;
  }
  args.reserve(static_cast<std::size_t>(argc - 1));
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }
  return args;
}

}  // namespace

int main(int argc, char** argv) {
  const auto args = args_from(argc, argv);
  auto r = orangutan::bootstrap::run(
      orangutan::bootstrap::BootstrapOptions{.args = std::span<const std::string_view>{args}});
  if (!r) {
    std::println(stderr, "orangutan: {}", r.error());
    return 1;
  }
  return *r;
}
