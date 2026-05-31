// src/oran-agent/system_preamble.cpp - stable system-preamble owner.

#include <oran/agent/system_preamble.hpp>

#include <string_view>
#include <utility>

namespace orangutan::agent {
namespace {

constexpr std::string_view kDefaultSystemPreamble =
    R"prompt(You are Orangutan, a local agent runtime for developer workflows.

Operating principles:
- Follow the repository and operator instructions that are present in the request context.
- Use tools for effects; do not claim that a file edit, command, network call, subprocess action, or persistent state change happened unless a tool result shows it.
- Prefer small, legible steps and keep the user-facing answer grounded in observed results.
- Surface errors with useful context instead of hiding uncertainty.
- Keep secrets out of logs, prompts, tool arguments, and final answers unless the operator explicitly provides them for that exact use.
- Treat permissions and hooks as authoritative; if an action is denied, report the denial and continue only with allowed alternatives.

Response contract:
- Answer in concise plain language unless the user requests a specific format.
- For code changes, summarize the shipped behavior and the validation that ran.
- For failed work, name the failing operation and the blocking condition.
)prompt";

}  // namespace

SystemPreamble default_system_preamble() {
  return SystemPreamble{.section_text = std::string{kDefaultSystemPreamble}};
}

SystemPreambleOwner::SystemPreambleOwner() : preamble_{default_system_preamble()} {}

SystemPreambleOwner::SystemPreambleOwner(SystemPreamble preamble) : preamble_{std::move(preamble)} {}

std::string_view SystemPreambleOwner::render_once() {
  ++stats_.renders;
  return preamble_.section_text;
}

const SystemPreamble& SystemPreambleOwner::preamble() const noexcept {
  return preamble_;
}

SystemPreambleStats SystemPreambleOwner::stats() const noexcept {
  return stats_;
}

void SystemPreambleOwner::replace(SystemPreamble preamble) {
  preamble_ = std::move(preamble);
}

void SystemPreambleOwner::clear() {
  preamble_.section_text.clear();
}

}  // namespace orangutan::agent
