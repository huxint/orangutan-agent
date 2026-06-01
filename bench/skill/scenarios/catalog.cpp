// bench/skill/scenarios/catalog.cpp
//
// A-vs-B coverage for section-4 skill-catalog rendering:
//
//   1. `skill.catalog_order_trusting_32` compares a local loader-order baseline.
//   2. `skill.catalog_deterministic_32` measures the production renderer's
//      validation plus deterministic name sort over the same 32 entries.

#include <nanobench.h>

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <oran/skill.hpp>

namespace orangutan::bench {
namespace {

[[nodiscard]] skill::CatalogEntry make_skill(std::uint32_t index) {
  return skill::CatalogEntry{
      .name = "skill." + std::to_string(31U - index),
      .description = "Synthetic prompt skill " + std::to_string(index) + ".",
      .triggers = {"trigger " + std::to_string(index), "intent " + std::to_string(index)},
      .model_hint = std::string{"keep output compact"},
  };
}

[[nodiscard]] std::vector<skill::CatalogEntry> make_catalog() {
  std::vector<skill::CatalogEntry> entries;
  entries.reserve(32);
  for (std::uint32_t i = 0; i < 32; ++i) {
    entries.push_back(make_skill(i));
  }
  return entries;
}

[[nodiscard]] std::string join_triggers(const std::vector<std::string>& triggers) {
  std::string out;
  for (std::size_t i = 0; i < triggers.size(); ++i) {
    if (i != 0) {
      out.append(", ");
    }
    out.append(triggers[i]);
  }
  return out;
}

[[nodiscard]] std::string render_order_trusting(const std::vector<skill::CatalogEntry>& entries) {
  std::string out;
  for (const auto& entry : entries) {
    if (!out.empty()) {
      out.append("\n\n");
    }
    out.append("Skill: ").append(entry.name).append("\n");
    out.append("Description: ").append(entry.description).append("\n");
    out.append("Triggers: ").append(entry.triggers.empty() ? std::string{"none"} : join_triggers(entry.triggers));
    if (entry.model_hint.has_value()) {
      out.append("\nModel Hint: ").append(*entry.model_hint);
    }
  }
  return out;
}

}  // namespace

void register_skill_catalog(ankerl::nanobench::Bench& bench) {
  const auto entries = make_catalog();
  const auto renderer = skill::CatalogRenderer{};

  bench.run("skill.catalog_order_trusting_32", [&] {
    auto rendered = render_order_trusting(entries);
    ankerl::nanobench::doNotOptimizeAway(rendered);
  });

  bench.run("skill.catalog_deterministic_32", [&] {
    auto rendered = renderer.render(entries);
    if (!rendered.has_value()) {
      std::abort();
    }
    ankerl::nanobench::doNotOptimizeAway(rendered->section_text);
  });
}

}  // namespace orangutan::bench
