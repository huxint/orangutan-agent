// src/oran-skill/catalog.cpp - deterministic skill-catalog rendering.

#include <oran/skill/catalog.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <oran/core/content.hpp>
#include <oran/core/error.hpp>
#include <oran/core/message.hpp>
#include <oran/core/role.hpp>

namespace orangutan::skill {
namespace {

constexpr std::string_view kSkillInvokeName{"skill.invoke"};
constexpr std::string_view kSkillDeactivateName{"skill.deactivate"};
constexpr std::string_view kActivationPrefix{R"({"kind":"skill_activation","version":1,"name":")"};
constexpr std::string_view kDeactivationPrefix{R"({"kind":"skill_deactivation","version":1,"name":")"};
constexpr std::string_view kActivationSuffix{R"("})"};

[[nodiscard]] bool contains_line_break(std::string_view value) noexcept {
  return value.contains('\n') || value.contains('\r');
}

[[nodiscard]] bool contains_control_char(std::string_view value) noexcept {
  return std::ranges::any_of(value, [](char c) { return static_cast<unsigned char>(c) < 0x20U; });
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

[[nodiscard]] core::Result<void> validate_active_skill(const ActiveSkill& skill) {
  if (is_blank(skill.name)) {
    return std::unexpected(core::Error::invalid_argument("active skill name must not be empty"));
  }
  if (contains_control_char(skill.name)) {
    return std::unexpected(core::Error::invalid_argument("active skill name must not contain control characters")
                               .with("skill", skill.name));
  }
  return validate_single_line("name", skill.name, skill.name);
}

[[nodiscard]] core::Result<std::vector<std::string_view>>
validate_deactivated_skill_names(std::span<const std::string> names) {
  std::vector<std::string_view> ordered;
  ordered.reserve(names.size());
  for (const auto& name : names) {
    if (auto valid = validate_active_skill(ActiveSkill{.name = name}); !valid) {
      return std::unexpected(std::move(valid).error());
    }
    ordered.emplace_back(name);
  }
  std::ranges::sort(ordered);
  for (std::size_t i = 1; i < ordered.size(); ++i) {
    if (ordered[i - 1] == ordered[i]) {
      return std::unexpected(core::Error::invalid_argument("deactivated skill names must be unique")
                                 .with("skill", std::string{ordered[i]}));
    }
  }
  return ordered;
}

[[nodiscard]] bool contains_active_skill(std::span<const ActiveSkill> active_skills,
                                         std::string_view skill_name) noexcept {
  return std::ranges::contains(active_skills, skill_name, [](const ActiveSkill& skill) -> std::string_view {
    return skill.name;
  });
}

[[nodiscard]] core::Result<std::vector<const SessionSkillActivation*>>
validate_session_skill_activations(std::span<const SessionSkillActivation> records) {
  std::vector<const SessionSkillActivation*> ordered;
  ordered.reserve(records.size());
  for (const auto& record : records) {
    if (auto valid = validate_active_skill(ActiveSkill{.name = record.name}); !valid) {
      return std::unexpected(std::move(valid).error());
    }
    ordered.push_back(&record);
  }
  std::ranges::sort(ordered, {}, [](const SessionSkillActivation* record) { return std::string_view{record->name}; });
  for (std::size_t i = 1; i < ordered.size(); ++i) {
    if (ordered[i - 1]->name == ordered[i]->name) {
      return std::unexpected(core::Error::invalid_argument("session skill activation names must be unique")
                                 .with("skill", ordered[i]->name));
    }
  }
  return ordered;
}

[[nodiscard]] core::Result<std::vector<std::string_view>> expired_skill_names(const ActivationPolicy& policy) {
  std::vector<std::string_view> ordered;
  ordered.reserve(policy.expirations.size());
  for (const auto& expiration : policy.expirations) {
    if (auto valid = validate_active_skill(ActiveSkill{.name = expiration.name}); !valid) {
      return std::unexpected(std::move(valid).error());
    }
    ordered.emplace_back(expiration.name);
  }
  std::ranges::sort(ordered);
  for (std::size_t i = 1; i < ordered.size(); ++i) {
    if (ordered[i - 1] == ordered[i]) {
      return std::unexpected(core::Error::invalid_argument("skill expiration names must be unique")
                                 .with("skill", std::string{ordered[i]}));
    }
  }

  if (!policy.expirations.empty() && !policy.evaluation_time.has_value()) {
    return std::unexpected(
        core::Error::invalid_argument("skill expiration policy requires an explicit evaluation time"));
  }

  std::vector<std::string_view> expired;
  expired.reserve(policy.expirations.size());
  if (!policy.evaluation_time.has_value()) {
    return expired;
  }
  for (const auto& expiration : policy.expirations) {
    if (expiration.expires_at <= *policy.evaluation_time) {
      expired.emplace_back(expiration.name);
    }
  }
  return expired;
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

void append_active_skill(std::string& output, const ActiveSkill& skill) {
  if (!output.empty()) {
    output.append("\n\n");
  }
  output.append("Active Skill: ").append(skill.name).append("\n");
  output.append("Status: active for this session");
}

void append_json_escaped(std::string& output, std::string_view value) {
  for (const auto ch : value) {
    switch (ch) {
      case '"':
        output.append(R"(\")");
        break;
      case '\\':
        output.append(R"(\\)");
        break;
      case '\b':
        output.append(R"(\b)");
        break;
      case '\f':
        output.append(R"(\f)");
        break;
      case '\n':
        output.append(R"(\n)");
        break;
      case '\r':
        output.append(R"(\r)");
        break;
      case '\t':
        output.append(R"(\t)");
        break;
      default:
        output.push_back(ch);
        break;
    }
  }
}

[[nodiscard]] std::optional<std::string> unescape_json_string(std::string_view value) {
  std::string output;
  output.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    const auto ch = value[i];
    if (ch != '\\') {
      output.push_back(ch);
      continue;
    }
    ++i;
    if (i == value.size()) {
      return std::nullopt;
    }
    switch (value[i]) {
      case '"':
        output.push_back('"');
        break;
      case '\\':
        output.push_back('\\');
        break;
      case '/':
        output.push_back('/');
        break;
      case 'b':
        output.push_back('\b');
        break;
      case 'f':
        output.push_back('\f');
        break;
      case 'n':
        output.push_back('\n');
        break;
      case 'r':
        output.push_back('\r');
        break;
      case 't':
        output.push_back('\t');
        break;
      default:
        return std::nullopt;
    }
  }
  return output;
}

}  // namespace

core::Result<RenderedCatalog> CatalogRenderer::render(std::span<const CatalogEntry> entries) const {
  return render(entries, std::span<const ActiveSkill>{});
}

core::Result<RenderedCatalog> CatalogRenderer::render(std::span<const CatalogEntry> entries,
                                                      std::span<const ActiveSkill> active_skills) const {
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

  std::vector<const ActiveSkill*> ordered_active;
  ordered_active.reserve(active_skills.size());
  for (const auto& skill : active_skills) {
    if (auto valid = validate_active_skill(skill); !valid) {
      return std::unexpected(std::move(valid).error());
    }
    ordered_active.push_back(&skill);
  }
  std::ranges::sort(ordered_active, {}, [](const ActiveSkill* skill) { return std::string_view{skill->name}; });
  for (std::size_t i = 1; i < ordered_active.size(); ++i) {
    if (ordered_active[i - 1]->name == ordered_active[i]->name) {
      return std::unexpected(
          core::Error::invalid_argument("active skill names must be unique").with("skill", ordered_active[i]->name));
    }
  }

  std::string section_text;
  for (const auto* skill : ordered_active) {
    section_text.reserve(section_text.size() + skill->name.size() + 64);
    append_active_skill(section_text, *skill);
  }
  for (const auto* entry : ordered) {
    section_text.reserve(section_text.size() + entry->name.size() + entry->description.size() + 96);
    append_entry(section_text, *entry);
  }

  return RenderedCatalog{.section_text = std::move(section_text)};
}

core::Result<RenderedCatalog> render_catalog(std::span<const CatalogEntry> entries) {
  return CatalogRenderer{}.render(entries);
}

core::Result<RenderedCatalog> render_catalog(std::span<const CatalogEntry> entries,
                                             std::span<const ActiveSkill> active_skills) {
  return CatalogRenderer{}.render(entries, active_skills);
}

core::Result<std::string> render_activation_data_json(std::string_view skill_name) {
  const auto active = ActiveSkill{.name = std::string{skill_name}};
  if (auto valid = validate_active_skill(active); !valid) {
    return std::unexpected(std::move(valid).error());
  }

  std::string output;
  output.reserve(kActivationPrefix.size() + skill_name.size() + kActivationSuffix.size());
  output.append(kActivationPrefix);
  append_json_escaped(output, skill_name);
  output.append(kActivationSuffix);
  return output;
}

std::optional<ActiveSkill> active_skill_from_data_json(std::string_view data_json) {
  if (!data_json.starts_with(kActivationPrefix) || !data_json.ends_with(kActivationSuffix)) {
    return std::nullopt;
  }
  data_json.remove_prefix(kActivationPrefix.size());
  data_json.remove_suffix(kActivationSuffix.size());
  auto name = unescape_json_string(data_json);
  if (!name.has_value()) {
    return std::nullopt;
  }
  auto active = ActiveSkill{.name = std::move(*name)};
  if (!validate_active_skill(active).has_value()) {
    return std::nullopt;
  }
  return active;
}

core::Result<std::string> render_deactivation_data_json(std::string_view skill_name) {
  const auto skill = ActiveSkill{.name = std::string{skill_name}};
  if (auto valid = validate_active_skill(skill); !valid) {
    return std::unexpected(std::move(valid).error());
  }

  std::string output;
  output.reserve(kDeactivationPrefix.size() + skill_name.size() + kActivationSuffix.size());
  output.append(kDeactivationPrefix);
  append_json_escaped(output, skill_name);
  output.append(kActivationSuffix);
  return output;
}

std::optional<std::string> deactivated_skill_from_data_json(std::string_view data_json) {
  if (!data_json.starts_with(kDeactivationPrefix) || !data_json.ends_with(kActivationSuffix)) {
    return std::nullopt;
  }
  data_json.remove_prefix(kDeactivationPrefix.size());
  data_json.remove_suffix(kActivationSuffix.size());
  auto name = unescape_json_string(data_json);
  if (!name.has_value() || !validate_active_skill(ActiveSkill{.name = *name}).has_value()) {
    return std::nullopt;
  }
  return name;
}

std::vector<ActiveSkill> active_skills_from_transcript(std::span<const core::Message> transcript) {
  std::unordered_map<std::string_view, std::string_view> tool_use_names;
  for (const auto& message : transcript) {
    if (message.role != core::Role::assistant) {
      continue;
    }
    for (const auto& block : message.blocks) {
      const auto* use = std::get_if<core::ToolUseContent>(&block);
      if (use != nullptr) {
        tool_use_names.emplace(use->id, use->name);
      }
    }
  }

  std::vector<ActiveSkill> active_skills;
  for (const auto& message : transcript) {
    if (message.role != core::Role::tool) {
      continue;
    }
    for (const auto& block : message.blocks) {
      const auto* result = std::get_if<core::ToolResultContent>(&block);
      if (result == nullptr || result->is_error || !result->data_json.has_value()) {
        continue;
      }
      const auto name = tool_use_names.find(result->tool_use_id);
      if (name == tool_use_names.end()) {
        continue;
      }
      if (name->second == kSkillInvokeName) {
        if (auto active = active_skill_from_data_json(*result->data_json);
            active.has_value() && !contains_active_skill(std::span<const ActiveSkill>{active_skills}, active->name)) {
          active_skills.push_back(std::move(*active));
        }
      } else if (name->second == kSkillDeactivateName) {
        if (auto deactivated = deactivated_skill_from_data_json(*result->data_json); deactivated.has_value()) {
          std::erase_if(active_skills, [&deactivated](const ActiveSkill& skill) { return skill.name == *deactivated; });
        }
      }
    }
  }
  return active_skills;
}

core::Result<std::vector<ActiveSkill>> resolve_active_skills(ActivationPolicy policy,
                                                             std::span<const core::Message> transcript,
                                                             std::span<const CatalogEntry> available_entries) {
  auto session_records =
      validate_session_skill_activations(std::span<const SessionSkillActivation>{policy.session_skill_activations});
  if (!session_records) {
    return std::unexpected(std::move(session_records).error());
  }
  auto deactivated_names =
      validate_deactivated_skill_names(std::span<const std::string>{policy.deactivated_skill_names});
  if (!deactivated_names) {
    return std::unexpected(std::move(deactivated_names).error());
  }
  auto expired_names = expired_skill_names(policy);
  if (!expired_names) {
    return std::unexpected(std::move(expired_names).error());
  }

  std::vector<const CatalogEntry*> ordered_entries;
  ordered_entries.reserve(available_entries.size());
  for (const auto& entry : available_entries) {
    if (auto valid = validate_entry(entry); !valid) {
      return std::unexpected(std::move(valid).error());
    }
    ordered_entries.push_back(&entry);
  }
  std::ranges::sort(ordered_entries, {}, [](const CatalogEntry* entry) { return std::string_view{entry->name}; });
  for (std::size_t i = 1; i < ordered_entries.size(); ++i) {
    if (ordered_entries[i - 1]->name == ordered_entries[i]->name) {
      return std::unexpected(core::Error::invalid_argument("skill catalog entry names must be unique")
                                 .with("skill", ordered_entries[i]->name));
    }
  }

  auto active_skills =
      policy.transcript_markers_enabled ? active_skills_from_transcript(transcript) : std::vector<ActiveSkill>{};
  for (const auto* record : *session_records) {
    if (record->active) {
      if (!contains_active_skill(std::span<const ActiveSkill>{active_skills}, record->name)) {
        active_skills.push_back(ActiveSkill{.name = record->name});
      }
    } else {
      std::erase_if(active_skills, [record](const ActiveSkill& skill) { return skill.name == record->name; });
    }
  }

  std::erase_if(active_skills, [&ordered_entries, &deactivated_names, &expired_names](const ActiveSkill& active) {
    const auto name = std::string_view{active.name};
    return !std::ranges::contains(ordered_entries,
                                  name,
                                  [](const CatalogEntry* entry) -> std::string_view { return entry->name; }) ||
           std::ranges::contains(*deactivated_names, name) || std::ranges::contains(*expired_names, name);
  });
  return active_skills;
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
