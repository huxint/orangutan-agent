// src/oran-skill/catalog.cpp - deterministic skill-catalog rendering.

#include <oran/skill/catalog.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <oran/core/error.hpp>

namespace orangutan::skill {
namespace {

[[nodiscard]] bool contains_line_break(std::string_view value) noexcept {
  return value.find('\n') != std::string_view::npos || value.find('\r') != std::string_view::npos;
}

[[nodiscard]] bool is_blank(std::string_view value) noexcept {
  return std::ranges::all_of(value, [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; });
}

[[nodiscard]] core::Result<void>
validate_single_line(std::string_view field, std::string_view value, std::string_view skill_name) {
  if (contains_line_break(value)) {
    return std::unexpected(core::Error::invalid_argument("skill catalog field must be single-line")
                               .with("field", std::string{field})
                               .with("skill", std::string{skill_name}));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_entry(const CatalogEntry& entry) {
  if (is_blank(entry.name)) {
    return std::unexpected(core::Error::invalid_argument("skill catalog entry name must not be empty"));
  }
  if (is_blank(entry.description)) {
    return std::unexpected(
        core::Error::invalid_argument("skill catalog entry description must not be empty").with("skill", entry.name));
  }

  if (auto valid = validate_single_line("name", entry.name, entry.name); !valid) {
    return valid;
  }
  if (auto valid = validate_single_line("description", entry.description, entry.name); !valid) {
    return valid;
  }
  for (const auto& trigger : entry.triggers) {
    if (is_blank(trigger)) {
      return std::unexpected(
          core::Error::invalid_argument("skill catalog trigger must not be empty").with("skill", entry.name));
    }
    if (auto valid = validate_single_line("trigger", trigger, entry.name); !valid) {
      return valid;
    }
  }
  if (entry.model_hint.has_value()) {
    if (is_blank(*entry.model_hint)) {
      return std::unexpected(
          core::Error::invalid_argument("skill catalog model hint must not be empty").with("skill", entry.name));
    }
    if (auto valid = validate_single_line("model_hint", *entry.model_hint, entry.name); !valid) {
      return valid;
    }
  }
  return {};
}

[[nodiscard]] std::string join_strings(std::span<const std::string> values, std::string_view separator) {
  std::size_t bytes = 0;
  for (const auto& value : values) {
    bytes += value.size() + separator.size();
  }
  if (!values.empty()) {
    bytes -= separator.size();
  }

  std::string output;
  output.reserve(bytes);
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      output.append(separator);
    }
    output.append(values[i]);
  }
  return output;
}

void append_entry(std::string& output, const CatalogEntry& entry) {
  if (!output.empty()) {
    output.append("\n\n");
  }
  output.append("Skill: ").append(entry.name).append("\n");
  output.append("Description: ").append(entry.description).append("\n");
  output.append("Triggers: ");
  if (entry.triggers.empty()) {
    output.append("none");
  } else {
    output.append(join_strings(std::span<const std::string>{entry.triggers}, ", "));
  }
  if (entry.model_hint.has_value()) {
    output.append("\nModel Hint: ").append(*entry.model_hint);
  }
}

}  // namespace

core::Result<RenderedCatalog> CatalogRenderer::render(std::span<const CatalogEntry> entries) const {
  std::vector<const CatalogEntry*> ordered;
  ordered.reserve(entries.size());
  for (const auto& entry : entries) {
    if (auto valid = validate_entry(entry); !valid) {
      return std::unexpected(std::move(valid).error());
    }
    ordered.push_back(&entry);
  }

  std::ranges::sort(ordered, {}, [](const CatalogEntry* entry) { return std::string_view{entry->name}; });
  for (std::size_t i = 1; i < ordered.size(); ++i) {
    if (ordered[i - 1]->name == ordered[i]->name) {
      return std::unexpected(
          core::Error::invalid_argument("skill catalog entry names must be unique").with("skill", ordered[i]->name));
    }
  }

  std::string section_text;
  for (const auto* entry : ordered) {
    section_text.reserve(section_text.size() + entry->name.size() + entry->description.size() + 96);
    append_entry(section_text, *entry);
  }

  return RenderedCatalog{.section_text = std::move(section_text)};
}

core::Result<RenderedCatalog> render_catalog(std::span<const CatalogEntry> entries) {
  return CatalogRenderer{}.render(entries);
}

CatalogOwner::CatalogOwner(RenderedCatalog catalog) : catalog_{std::move(catalog)} {}

std::string_view CatalogOwner::render_once() {
  ++stats_.renders;
  return catalog_.section_text;
}

const RenderedCatalog& CatalogOwner::catalog() const noexcept {
  return catalog_;
}

CatalogStats CatalogOwner::stats() const noexcept {
  return stats_;
}

void CatalogOwner::replace(RenderedCatalog catalog) {
  catalog_ = std::move(catalog);
}

void CatalogOwner::clear() {
  catalog_.section_text.clear();
}

}  // namespace orangutan::skill
