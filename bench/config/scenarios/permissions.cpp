// bench/config/scenarios/permissions.cpp
//
// A-vs-B comparison: parse an empty permissions block vs. a populated
// 16-rule block with one agent overlay vs. the same 16-rule block where
// four `deny` rules carry an `input_pattern` re2 pattern. The populated
// path is the cost the future `oran-permission::materialize` consumer
// pays at startup; the empty path documents the parse-side surface lift
// (a no-op permissions block should not cost meaningfully more than a
// config without one); the input_pattern path documents the additional
// re2 compile cost per pattern that the load-time validator pays.

#include <nanobench.h>

#include <cstdlib>
#include <string_view>

#include <oran/config.hpp>

namespace orangutan::bench {
namespace config = orangutan::config;

namespace {

constexpr auto kEmptyPermissionsConfig = std::string_view{R"json(
{
  "runtime": {"workers": 1},
  "permissions": {}
}
)json"};

constexpr auto kTypedPermissionsConfig = std::string_view{R"json(
{
  "runtime": {"workers": 1},
  "permissions": {
    "allow": [
      {"tool_pattern": "FileRead"},
      {"tool_pattern": "FileSearch"},
      {"tool_pattern": "*", "capability": "read_file"},
      {"tool_pattern": "*", "capability": "read_memory"},
      {"tool_pattern": "*", "capability": "egress_http"}
    ],
    "deny": [
      {"tool_pattern": "*", "capability": "runtime_loader"},
      {"tool_pattern": "*", "capability": "delete_path"},
      {"tool_pattern": "ShellExec(rm:*)"}
    ],
    "ask": [
      {"tool_pattern": "FileWrite"},
      {"tool_pattern": "FileEdit"},
      {"tool_pattern": "*", "capability": "write_file"},
      {"tool_pattern": "*", "capability": "edit_file"},
      {"tool_pattern": "*", "capability": "spawn_subprocess"},
      {"tool_pattern": "*", "capability": "write_memory"},
      {"tool_pattern": "*", "capability": "egress_websocket"},
      {"tool_pattern": "*", "capability": "schedule_job"}
    ]
  },
  "agents": {
    "researcher": {
      "permissions": {
        "allow": [
          {"tool_pattern": "*", "capability": "egress_http"},
          {"tool_pattern": "*", "capability": "read_file"}
        ]
      }
    }
  }
}
)json"};

constexpr auto kInputPatternPermissionsConfig = std::string_view{R"json(
{
  "runtime": {"workers": 1},
  "permissions": {
    "allow": [
      {"tool_pattern": "FileRead"},
      {"tool_pattern": "FileSearch"},
      {"tool_pattern": "*", "capability": "read_file"},
      {"tool_pattern": "*", "capability": "read_memory"},
      {"tool_pattern": "*", "capability": "egress_http"}
    ],
    "deny": [
      {"tool_pattern": "*", "capability": "runtime_loader"},
      {"tool_pattern": "*", "capability": "delete_path"},
      {"tool_pattern": "ShellExec", "input_pattern": "^rm -rf"},
      {"tool_pattern": "ShellExec", "input_pattern": "^git push"},
      {"tool_pattern": "ShellExec", "input_pattern": "^sudo"},
      {"tool_pattern": "ShellExec", "input_pattern": "; *rm "}
    ],
    "ask": [
      {"tool_pattern": "FileWrite"},
      {"tool_pattern": "FileEdit"},
      {"tool_pattern": "*", "capability": "write_file"},
      {"tool_pattern": "*", "capability": "edit_file"},
      {"tool_pattern": "*", "capability": "spawn_subprocess"}
    ]
  }
}
)json"};

[[gnu::noinline]] std::size_t parse_empty_permissions() {
  auto parsed = config::Config::parse(kEmptyPermissionsConfig);
  if (!parsed) {
    std::abort();
  }
  return parsed->permissions().rules.size() + parsed->agents().size();
}

[[gnu::noinline]] std::size_t parse_typed_permissions() {
  auto parsed = config::Config::parse(kTypedPermissionsConfig);
  if (!parsed) {
    std::abort();
  }
  return parsed->permissions().rules.size() + parsed->agents().size();
}

[[gnu::noinline]] std::size_t parse_input_pattern_permissions() {
  auto parsed = config::Config::parse(kInputPatternPermissionsConfig);
  if (!parsed) {
    std::abort();
  }
  return parsed->permissions().rules.size();
}

}  // namespace

void register_permissions(ankerl::nanobench::Bench& bench) {
  bench.run("config.parse_permissions_empty", [] {
    const auto value = parse_empty_permissions();
    ankerl::nanobench::doNotOptimizeAway(value);
  });
  bench.run("config.parse_permissions_typed", [] {
    const auto value = parse_typed_permissions();
    ankerl::nanobench::doNotOptimizeAway(value);
  });
  bench.run("config.parse_permissions_with_input_patterns", [] {
    const auto value = parse_input_pattern_permissions();
    ankerl::nanobench::doNotOptimizeAway(value);
  });
}

}  // namespace orangutan::bench
