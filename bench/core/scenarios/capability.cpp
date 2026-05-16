// bench/core/scenarios/capability.cpp
//
// A-vs-B coverage for `core::parse_capability`:
//
//   1. `core.capability_parse_linear`        : `core::parse_capability` —
//                                              a constexpr-table linear
//                                              scan over the 19-entry
//                                              capability universe.
//   2. `core.capability_parse_unordered_map` : the same lookup driven by
//                                              an `std::unordered_map<
//                                              std::string_view,
//                                              Capability>` keyed on the
//                                              same spellings.
//
// Both scenarios iterate the same deterministic mix of valid and invalid
// inputs so the comparison documents the cost of the table-scan path the
// library actually ships vs. the cheapest hash-table alternative. The
// goal is *not* to crown a winner — at 19 entries the linear scan wins on
// every realistic toolchain — but to give future callers (config-loading
// large rule lists, schema generation) a recorded baseline so they reach
// for a different data structure with eyes open.

#include <nanobench.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <oran/core/capability.hpp>

namespace orangutan::bench {

namespace {

using core::Capability;
using core::kAllCapabilities;
using core::parse_capability;
using core::to_string_view;

// A deterministic mix of: every valid spelling once, three high-traffic
// spellings repeated, two unknown spellings. The map and the linear scan
// see byte-for-byte identical inputs.
[[nodiscard]] std::vector<std::string> make_lookup_inputs() {
  std::vector<std::string> inputs;
  for (const auto cap : kAllCapabilities()) {
    inputs.emplace_back(to_string_view(cap));
  }
  inputs.emplace_back("read_file");
  inputs.emplace_back("spawn_subprocess");
  inputs.emplace_back("runtime_loader");
  inputs.emplace_back("");
  inputs.emplace_back("not_a_capability");
  return inputs;
}

[[nodiscard]] std::unordered_map<std::string_view, Capability> make_map() {
  std::unordered_map<std::string_view, Capability> m;
  m.reserve(kAllCapabilities().size());
  for (const auto cap : kAllCapabilities()) {
    m.emplace(to_string_view(cap), cap);
  }
  return m;
}

}  // namespace

void register_capability_scenarios(ankerl::nanobench::Bench& bench) {
  static const std::vector<std::string> inputs = make_lookup_inputs();
  static const std::unordered_map<std::string_view, Capability> map = make_map();

  bench.run("core.capability_parse_linear", [&] {
    int hits = 0;
    for (const auto& key : inputs) {
      if (parse_capability(key).has_value()) {
        ++hits;
      }
    }
    ankerl::nanobench::doNotOptimizeAway(hits);
  });
  bench.run("core.capability_parse_unordered_map", [&] {
    int hits = 0;
    for (const auto& key : inputs) {
      if (map.find(std::string_view{key}) != map.end()) {
        ++hits;
      }
    }
    ankerl::nanobench::doNotOptimizeAway(hits);
  });
}

}  // namespace orangutan::bench
