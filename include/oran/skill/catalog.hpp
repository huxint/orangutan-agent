// include/oran/skill/catalog.hpp - compact prompt-facing skill catalog.
//
// `oran-skill` owns the prompt section-4 bytes. Callers provide, or load, a
// metadata snapshot plus optional active-skill markers and receive compact,
// deterministic catalog text. Skill bodies are intentionally absent from this
// renderer; loader/invoke work snapshots bodies without moving them into the
// stable system preamble.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <oran/core/result.hpp>

namespace orangutan::core {
struct Message;
}  // namespace orangutan::core

namespace orangutan::skill {

struct CatalogEntry {
  std::string name;
  std::string description;
  std::vector<std::string> triggers;
  std::optional<std::string> model_hint;

  friend bool operator==(const CatalogEntry&, const CatalogEntry&) = default;
};

struct ActiveSkill {
  std::string name;

  friend bool operator==(const ActiveSkill&, const ActiveSkill&) = default;
};

struct ActivationPolicy {
  /// Current v1 policy: successful `skill.invoke` tool results in the session
  /// transcript mark the skill active on the next prompt. Future expiration or
  /// explicit deactivation rules should extend this value instead of parsing
  /// transcript state at bootstrap call sites.
  bool transcript_markers_enabled{true};

  friend bool operator==(const ActivationPolicy&, const ActivationPolicy&) = default;
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
  [[nodiscard]] core::Result<RenderedCatalog> render(std::span<const CatalogEntry> entries,
                                                     std::span<const ActiveSkill> active_skills) const;
};

[[nodiscard]] core::Result<RenderedCatalog> render_catalog(std::span<const CatalogEntry> entries);
[[nodiscard]] core::Result<RenderedCatalog> render_catalog(std::span<const CatalogEntry> entries,
                                                           std::span<const ActiveSkill> active_skills);

/// Versioned structured metadata emitted by `skill.invoke` on successful
/// dispatch. It is small enough to travel in `ToolResultContent::data_json`;
/// the text body remains the model-visible skill template.
[[nodiscard]] core::Result<std::string> render_activation_data_json(std::string_view skill_name);
[[nodiscard]] std::optional<ActiveSkill> active_skill_from_data_json(std::string_view data_json);
[[nodiscard]] std::vector<ActiveSkill> active_skills_from_transcript(std::span<const core::Message> transcript);
[[nodiscard]] core::Result<std::vector<ActiveSkill>>
resolve_active_skills(ActivationPolicy policy,
                      std::span<const core::Message> transcript,
                      std::span<const CatalogEntry> available_entries);

/// Owner for section-4 prompt skill-catalog bytes.
///
/// It owns already-rendered section text so bootstrap can render the prompt
/// section exactly once before a multi-iteration agent loop. Loader, watcher,
/// and `skill.invoke` consumers can replace the catalog snapshot without
/// changing the loop or prompt-builder boundary.
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
