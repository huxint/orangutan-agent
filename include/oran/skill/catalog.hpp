// include/oran/skill/catalog.hpp - compact prompt-facing skill catalog.
//
// `oran-skill` owns the prompt section-4 bytes. The current slice keeps the
// surface deliberately small: callers provide a metadata snapshot and receive
// compact, deterministic catalog text. Skill bodies are intentionally absent
// from this API; future loader/invoke work can snapshot bodies without moving
// them into the stable system preamble.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <oran/core/result.hpp>

namespace orangutan::skill {

struct CatalogEntry {
  std::string name;
  std::string description;
  std::vector<std::string> triggers;
  std::optional<std::string> model_hint;

  friend bool operator==(const CatalogEntry&, const CatalogEntry&) = default;
};

struct RenderedCatalog {
  std::string section_text;

  friend bool operator==(const RenderedCatalog&, const RenderedCatalog&) = default;
};

struct CatalogStats {
  std::uint64_t renders{0};

  friend bool operator==(const CatalogStats&, const CatalogStats&) = default;
};

class CatalogRenderer {
public:
  [[nodiscard]] core::Result<RenderedCatalog> render(std::span<const CatalogEntry> entries) const;
};

[[nodiscard]] core::Result<RenderedCatalog> render_catalog(std::span<const CatalogEntry> entries);

/// Owner for section-4 prompt skill-catalog bytes.
///
/// It owns already-rendered section text so bootstrap can render the prompt
/// section exactly once before a multi-iteration agent loop. Loader, watcher,
/// and `skill.invoke` slices can replace the catalog snapshot without changing
/// the loop or prompt-builder boundary.
class CatalogOwner {
public:
  explicit CatalogOwner(RenderedCatalog catalog = {});

  [[nodiscard]] std::string_view render_once();
  [[nodiscard]] const RenderedCatalog& catalog() const noexcept;
  [[nodiscard]] CatalogStats stats() const noexcept;
  void replace(RenderedCatalog catalog);
  void clear();

private:
  RenderedCatalog catalog_{};
  CatalogStats stats_{};
};

}  // namespace orangutan::skill
