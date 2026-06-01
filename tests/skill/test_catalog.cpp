// tests/skill/test_catalog.cpp - skill catalog renderer coverage.

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/content.hpp>
#include <oran/core/error.hpp>
#include <oran/core/message.hpp>
#include <oran/core/role.hpp>
#include <oran/skill.hpp>

namespace core = orangutan::core;
namespace skill = orangutan::skill;

TEST_CASE("CatalogRenderer permits an empty skill catalog", "[unit][skill][catalog]") {
  const auto rendered = skill::render_catalog(std::span<const skill::CatalogEntry>{});

  REQUIRE(rendered.has_value());
  REQUIRE(rendered->section_text.empty());
}

TEST_CASE("CatalogRenderer renders deterministic compact skill entries", "[unit][skill][catalog]") {
  const std::vector<skill::CatalogEntry> entries{
      skill::CatalogEntry{
          .name = "review-pr",
          .description = "Review code changes for regressions.",
          .triggers = {"code review", "pull request"},
          .model_hint = std::string{"cite concrete file paths"},
      },
      skill::CatalogEntry{
          .name = "release-note",
          .description = "Draft release notes from completed changes.",
          .triggers = {"release notes", "changelog"},
          .model_hint = std::nullopt,
      },
  };

  const auto rendered = skill::render_catalog(entries);

  REQUIRE(rendered.has_value());
  REQUIRE(rendered->section_text.starts_with("Skill: release-note\n"));
  REQUIRE(rendered->section_text.find("Skill: release-note") < rendered->section_text.find("Skill: review-pr"));
  REQUIRE(rendered->section_text.contains("Description: Draft release notes from completed changes.\n"));
  REQUIRE(rendered->section_text.contains("Triggers: release notes, changelog"));
  REQUIRE(rendered->section_text.contains("Model Hint: cite concrete file paths"));
  REQUIRE_FALSE(rendered->section_text.contains("Body:"));
}

TEST_CASE("CatalogRenderer renders deterministic active skill markers", "[unit][skill][catalog]") {
  const std::vector<skill::CatalogEntry> entries{
      skill::CatalogEntry{
          .name = "release-note",
          .description = "Draft release notes from completed changes.",
          .triggers = {"release notes"},
          .model_hint = std::nullopt,
      },
  };
  const std::vector<skill::ActiveSkill> active{
      skill::ActiveSkill{.name = "review-pr"},
      skill::ActiveSkill{.name = "release-note"},
  };

  const auto rendered = skill::render_catalog(entries, active);

  REQUIRE(rendered.has_value());
  REQUIRE(rendered->section_text.starts_with("Active Skill: release-note\n"));
  REQUIRE(rendered->section_text.find("Active Skill: release-note") <
          rendered->section_text.find("Active Skill: review-pr"));
  REQUIRE(rendered->section_text.find("Active Skill: review-pr") <
          rendered->section_text.find("\n\nSkill: release-note"));
  REQUIRE(rendered->section_text.contains("Status: active for this session"));
  REQUIRE(rendered->section_text.contains("Skill: release-note"));
}

TEST_CASE("CatalogRenderer rejects duplicate active skill markers", "[unit][skill][catalog]") {
  const std::vector<skill::ActiveSkill> active{
      skill::ActiveSkill{.name = "release-note"},
      skill::ActiveSkill{.name = "release-note"},
  };

  const auto rendered = skill::render_catalog(std::span<const skill::CatalogEntry>{}, active);

  REQUIRE_FALSE(rendered.has_value());
  REQUIRE(rendered.error().kind() == core::ErrorKind::invalid_argument);
}

TEST_CASE("CatalogRenderer rejects duplicate skill names", "[unit][skill][catalog]") {
  const std::vector<skill::CatalogEntry> entries{
      skill::CatalogEntry{
          .name = "release-note",
          .description = "Draft release notes.",
          .triggers = {},
          .model_hint = std::nullopt,
      },
      skill::CatalogEntry{
          .name = "release-note",
          .description = "Draft another note.",
          .triggers = {},
          .model_hint = std::nullopt,
      },
  };

  const auto rendered = skill::render_catalog(entries);

  REQUIRE_FALSE(rendered.has_value());
  REQUIRE(rendered.error().kind() == core::ErrorKind::invalid_argument);
}

TEST_CASE("CatalogRenderer rejects blank or multiline metadata", "[unit][skill][catalog]") {
  const std::vector<skill::CatalogEntry> blank_name{skill::CatalogEntry{
      .name = " ",
      .description = "desc",
      .triggers = {},
      .model_hint = std::nullopt,
  }};
  const auto blank_result = skill::render_catalog(blank_name);
  REQUIRE_FALSE(blank_result.has_value());
  REQUIRE(blank_result.error().kind() == core::ErrorKind::invalid_argument);

  const std::vector<skill::CatalogEntry> multiline_trigger{skill::CatalogEntry{
      .name = "release-note",
      .description = "Draft release notes.",
      .triggers = {"release\nnotes"},
      .model_hint = std::nullopt,
  }};
  const auto multiline_result = skill::render_catalog(multiline_trigger);
  REQUIRE_FALSE(multiline_result.has_value());
  REQUIRE(multiline_result.error().kind() == core::ErrorKind::invalid_argument);
}

TEST_CASE("Skill activation metadata round-trips through transcripts", "[unit][skill][catalog]") {
  auto activation = skill::render_activation_data_json("release-note");
  REQUIRE(activation.has_value());
  REQUIRE(*activation == R"({"kind":"skill_activation","version":1,"name":"release-note"})");

  const std::vector<core::Message> transcript{
      core::Message{
          .role = core::Role::assistant,
          .blocks = {core::ToolUseContent{
              .id = "skill-1",
              .name = "skill.invoke",
              .input_json = R"({"name":"release-note"})",
          }},
          .created_at = std::nullopt,
      },
      core::Message{
          .role = core::Role::tool,
          .blocks = {core::ToolResultContent{
              .tool_use_id = "skill-1",
              .output = "body",
              .data_json = *activation,
              .is_error = false,
          }},
          .created_at = std::nullopt,
      },
      core::Message{
          .role = core::Role::tool,
          .blocks = {core::ToolResultContent{
              .tool_use_id = "skill-1",
              .output = "duplicate",
              .data_json = *activation,
              .is_error = false,
          }},
          .created_at = std::nullopt,
      },
  };

  const auto active = skill::active_skills_from_transcript(transcript);

  REQUIRE(active == std::vector<skill::ActiveSkill>{skill::ActiveSkill{.name = "release-note"}});
  REQUIRE(skill::active_skill_from_data_json(*activation) == skill::ActiveSkill{.name = "release-note"});
  REQUIRE_FALSE(
      skill::active_skill_from_data_json(R"({"kind":"other","version":1,"name":"release-note"})").has_value());
}

TEST_CASE("CatalogOwner renders, replaces, and clears section state", "[unit][skill][catalog]") {
  skill::CatalogOwner owner{skill::RenderedCatalog{.section_text = "Skill: old"}};

  REQUIRE(owner.render_once() == std::string_view{"Skill: old"});

  owner.replace(skill::RenderedCatalog{.section_text = "Skill: new"});
  REQUIRE(owner.render_once() == std::string_view{"Skill: new"});

  owner.clear();
  REQUIRE(owner.render_once().empty());
  REQUIRE(owner.stats().renders == 3);
}
