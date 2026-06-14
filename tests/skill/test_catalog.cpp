// tests/skill/test_catalog.cpp - skill catalog renderer coverage.

#include <chrono>
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
              .name = "SkillInvoke",
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

TEST_CASE("Skill deactivation metadata round-trips and stays distinct from activation", "[unit][skill][catalog]") {
  auto deactivation = skill::render_deactivation_data_json("release-note");
  REQUIRE(deactivation.has_value());
  REQUIRE(*deactivation == R"({"kind":"skill_deactivation","version":1,"name":"release-note"})");
  REQUIRE(skill::deactivated_skill_from_data_json(*deactivation) == std::optional<std::string>{"release-note"});

  auto activation = skill::render_activation_data_json("release-note");
  REQUIRE(activation.has_value());
  // The two record kinds never parse as each other.
  REQUIRE_FALSE(skill::deactivated_skill_from_data_json(*activation).has_value());
  REQUIRE_FALSE(skill::active_skill_from_data_json(*deactivation).has_value());
  // Blank or multiline names are rejected at render time.
  REQUIRE_FALSE(skill::render_deactivation_data_json(" ").has_value());
  REQUIRE_FALSE(skill::render_deactivation_data_json("release\nnote").has_value());
}

namespace {

[[nodiscard]] core::Message skill_tool_use(std::string_view id, std::string_view tool_name) {
  return core::Message{
      .role = core::Role::assistant,
      .blocks = {core::ToolUseContent{.id = std::string{id}, .name = std::string{tool_name}, .input_json = "{}"}},
      .created_at = std::nullopt,
  };
}

[[nodiscard]] core::Message skill_tool_result(std::string_view id, std::string_view data_json) {
  return core::Message{
      .role = core::Role::tool,
      .blocks = {core::ToolResultContent{.tool_use_id = std::string{id},
                                         .output = "ok",
                                         .data_json = std::string{data_json},
                                         .is_error = false}},
      .created_at = std::nullopt,
  };
}

[[nodiscard]] core::Message skill_tool_error_result(std::string_view id, std::string_view data_json) {
  return core::Message{
      .role = core::Role::tool,
      .blocks = {core::ToolResultContent{.tool_use_id = std::string{id},
                                         .output = "error",
                                         .data_json = std::string{data_json},
                                         .is_error = true}},
      .created_at = std::nullopt,
  };
}

}  // namespace

TEST_CASE("skill_activation_events_from_transcript extracts suffix activation updates", "[unit][skill][catalog]") {
  auto old_activation = skill::render_activation_data_json("old-skill");
  auto activation = skill::render_activation_data_json("release-note");
  auto deactivation = skill::render_deactivation_data_json("release-note");
  REQUIRE(old_activation.has_value());
  REQUIRE(activation.has_value());
  REQUIRE(deactivation.has_value());

  const std::vector<core::Message> transcript{
      skill_tool_use("old-1", "SkillInvoke"),
      skill_tool_result("old-1", *old_activation),
      skill_tool_use("s1", "SkillInvoke"),
      skill_tool_result("s1", *activation),
      skill_tool_use("s2", "FileRead"),
      skill_tool_result("s2", *activation),
      skill_tool_use("s3", "SkillInvoke"),
      skill_tool_error_result("s3", *activation),
      skill_tool_use("s4", "SkillDeactivate"),
      skill_tool_result("s4", *deactivation),
  };

  const auto events = skill::skill_activation_events_from_transcript(transcript, 2);

  REQUIRE(events == std::vector<skill::SkillActivationEvent>{
                        skill::SkillActivationEvent{.name = "release-note", .active = true},
                        skill::SkillActivationEvent{.name = "release-note", .active = false},
                    });
  REQUIRE(skill::skill_activation_events_from_transcript(transcript, transcript.size() + 1).empty());
}

TEST_CASE("active_skills_from_transcript nets SkillDeactivate against SkillInvoke", "[unit][skill][catalog]") {
  auto activation = skill::render_activation_data_json("release-note");
  auto deactivation = skill::render_deactivation_data_json("release-note");
  REQUIRE(activation.has_value());
  REQUIRE(deactivation.has_value());

  SECTION("invoke then deactivate leaves the skill inactive") {
    const std::vector<core::Message> transcript{
        skill_tool_use("s1", "SkillInvoke"),
        skill_tool_result("s1", *activation),
        skill_tool_use("s2", "SkillDeactivate"),
        skill_tool_result("s2", *deactivation),
    };
    REQUIRE(skill::active_skills_from_transcript(transcript).empty());
  }

  SECTION("a later invoke reactivates the skill (most recent event wins)") {
    const std::vector<core::Message> transcript{
        skill_tool_use("s1", "SkillInvoke"),
        skill_tool_result("s1", *activation),
        skill_tool_use("s2", "SkillDeactivate"),
        skill_tool_result("s2", *deactivation),
        skill_tool_use("s3", "SkillInvoke"),
        skill_tool_result("s3", *activation),
    };
    REQUIRE(skill::active_skills_from_transcript(transcript) ==
            std::vector<skill::ActiveSkill>{skill::ActiveSkill{.name = "release-note"}});
  }

  SECTION("deactivating a never-invoked skill is a harmless no-op") {
    const std::vector<core::Message> transcript{
        skill_tool_use("s1", "SkillDeactivate"),
        skill_tool_result("s1", *deactivation),
    };
    REQUIRE(skill::active_skills_from_transcript(transcript).empty());
  }
}

TEST_CASE("resolve_active_skills honours transcript SkillDeactivate", "[unit][skill][catalog]") {
  auto release_activation = skill::render_activation_data_json("release-note");
  auto review_activation = skill::render_activation_data_json("review-pr");
  auto release_deactivation = skill::render_deactivation_data_json("release-note");
  REQUIRE(release_activation.has_value());
  REQUIRE(review_activation.has_value());
  REQUIRE(release_deactivation.has_value());

  const std::vector<core::Message> transcript{
      skill_tool_use("s1", "SkillInvoke"),
      skill_tool_result("s1", *release_activation),
      skill_tool_use("s2", "SkillInvoke"),
      skill_tool_result("s2", *review_activation),
      skill_tool_use("s3", "SkillDeactivate"),
      skill_tool_result("s3", *release_deactivation),
  };
  const std::vector<skill::CatalogEntry> available{
      skill::CatalogEntry{
          .name = "release-note",
          .description = "Draft release notes.",
          .triggers = {},
          .model_hint = std::nullopt,
      },
      skill::CatalogEntry{
          .name = "review-pr",
          .description = "Review code changes.",
          .triggers = {},
          .model_hint = std::nullopt,
      },
  };

  const auto active = skill::resolve_active_skills(skill::ActivationPolicy{},
                                                   std::span<const core::Message>{transcript},
                                                   std::span<const skill::CatalogEntry>{available});

  REQUIRE(active.has_value());
  REQUIRE(*active == std::vector<skill::ActiveSkill>{skill::ActiveSkill{.name = "review-pr"}});
}

TEST_CASE("ActivationPolicy resolves active markers against available skills", "[unit][skill][catalog]") {
  auto release_activation = skill::render_activation_data_json("release-note");
  auto missing_activation = skill::render_activation_data_json("removed-skill");
  REQUIRE(release_activation.has_value());
  REQUIRE(missing_activation.has_value());
  const std::vector<core::Message> transcript{
      core::Message{
          .role = core::Role::assistant,
          .blocks =
              {
                  core::ToolUseContent{
                      .id = "skill-1",
                      .name = "SkillInvoke",
                      .input_json = R"({"name":"release-note"})",
                  },
                  core::ToolUseContent{
                      .id = "skill-2",
                      .name = "SkillInvoke",
                      .input_json = R"({"name":"removed-skill"})",
                  },
              },
          .created_at = std::nullopt,
      },
      core::Message{
          .role = core::Role::tool,
          .blocks =
              {
                  core::ToolResultContent{
                      .tool_use_id = "skill-1",
                      .output = "release body",
                      .data_json = *release_activation,
                      .is_error = false,
                  },
                  core::ToolResultContent{
                      .tool_use_id = "skill-2",
                      .output = "removed body",
                      .data_json = *missing_activation,
                      .is_error = false,
                  },
              },
          .created_at = std::nullopt,
      },
  };
  const std::vector<skill::CatalogEntry> available{
      skill::CatalogEntry{
          .name = "release-note",
          .description = "Draft release notes.",
          .triggers = {},
          .model_hint = std::nullopt,
      },
  };

  const auto active = skill::resolve_active_skills(skill::ActivationPolicy{},
                                                   std::span<const core::Message>{transcript},
                                                   std::span<const skill::CatalogEntry>{available});

  REQUIRE(active.has_value());
  REQUIRE(*active == std::vector<skill::ActiveSkill>{skill::ActiveSkill{.name = "release-note"}});
}

TEST_CASE("ActivationPolicy can disable transcript-derived active markers", "[unit][skill][catalog]") {
  auto activation = skill::render_activation_data_json("release-note");
  REQUIRE(activation.has_value());
  const std::vector<core::Message> transcript{
      core::Message{
          .role = core::Role::assistant,
          .blocks = {core::ToolUseContent{
              .id = "skill-1",
              .name = "SkillInvoke",
              .input_json = R"({"name":"release-note"})",
          }},
          .created_at = std::nullopt,
      },
      core::Message{
          .role = core::Role::tool,
          .blocks = {core::ToolResultContent{
              .tool_use_id = "skill-1",
              .output = "release body",
              .data_json = *activation,
              .is_error = false,
          }},
          .created_at = std::nullopt,
      },
  };
  const std::vector<skill::CatalogEntry> available{
      skill::CatalogEntry{
          .name = "release-note",
          .description = "Draft release notes.",
          .triggers = {},
          .model_hint = std::nullopt,
      },
  };

  const auto active = skill::resolve_active_skills(skill::ActivationPolicy{.transcript_markers_enabled = false},
                                                   std::span<const core::Message>{transcript},
                                                   std::span<const skill::CatalogEntry>{available});

  REQUIRE(active.has_value());
  REQUIRE(active->empty());
}

TEST_CASE("ActivationPolicy overlays session-store activation records", "[unit][skill][catalog]") {
  auto release_activation = skill::render_activation_data_json("release-note");
  auto review_activation = skill::render_activation_data_json("review-pr");
  REQUIRE(release_activation.has_value());
  REQUIRE(review_activation.has_value());
  const std::vector<core::Message> transcript{
      skill_tool_use("s1", "SkillInvoke"),
      skill_tool_result("s1", *release_activation),
      skill_tool_use("s2", "SkillInvoke"),
      skill_tool_result("s2", *review_activation),
  };
  const std::vector<skill::CatalogEntry> available{
      skill::CatalogEntry{
          .name = "release-note",
          .description = "Draft release notes.",
          .triggers = {},
          .model_hint = std::nullopt,
      },
      skill::CatalogEntry{
          .name = "review-pr",
          .description = "Review code changes.",
          .triggers = {},
          .model_hint = std::nullopt,
      },
      skill::CatalogEntry{
          .name = "summarize-doc",
          .description = "Summarize a document.",
          .triggers = {},
          .model_hint = std::nullopt,
      },
  };

  const auto active = skill::resolve_active_skills(
      skill::ActivationPolicy{
          .session_skill_activations =
              {
                  skill::SessionSkillActivation{.name = "release-note", .active = false},
                  skill::SessionSkillActivation{.name = "summarize-doc", .active = true},
              },
      },
      std::span<const core::Message>{transcript},
      std::span<const skill::CatalogEntry>{available});

  REQUIRE(active.has_value());
  REQUIRE(*active == std::vector<skill::ActiveSkill>{skill::ActiveSkill{.name = "review-pr"},
                                                     skill::ActiveSkill{.name = "summarize-doc"}});
}

TEST_CASE("ActivationPolicy validates session-store activation records", "[unit][skill][catalog]") {
  const std::vector<skill::CatalogEntry> available{
      skill::CatalogEntry{
          .name = "release-note",
          .description = "Draft release notes.",
          .triggers = {},
          .model_hint = std::nullopt,
      },
  };

  const auto duplicate = skill::resolve_active_skills(
      skill::ActivationPolicy{
          .session_skill_activations =
              {
                  skill::SessionSkillActivation{.name = "release-note", .active = true},
                  skill::SessionSkillActivation{.name = "release-note", .active = false},
              },
      },
      std::span<const core::Message>{},
      std::span<const skill::CatalogEntry>{available});
  REQUIRE_FALSE(duplicate.has_value());
  REQUIRE(duplicate.error().kind() == core::ErrorKind::invalid_argument);

  const auto blank = skill::resolve_active_skills(
      skill::ActivationPolicy{
          .session_skill_activations = {skill::SessionSkillActivation{.name = " ", .active = true}},
      },
      std::span<const core::Message>{},
      std::span<const skill::CatalogEntry>{available});
  REQUIRE_FALSE(blank.has_value());
  REQUIRE(blank.error().kind() == core::ErrorKind::invalid_argument);
}

TEST_CASE("ActivationPolicy deactivates transcript-derived active markers", "[unit][skill][catalog]") {
  auto release_activation = skill::render_activation_data_json("release-note");
  auto review_activation = skill::render_activation_data_json("review-pr");
  REQUIRE(release_activation.has_value());
  REQUIRE(review_activation.has_value());
  const std::vector<core::Message> transcript{
      core::Message{
          .role = core::Role::assistant,
          .blocks =
              {
                  core::ToolUseContent{
                      .id = "skill-1",
                      .name = "SkillInvoke",
                      .input_json = R"({"name":"release-note"})",
                  },
                  core::ToolUseContent{
                      .id = "skill-2",
                      .name = "SkillInvoke",
                      .input_json = R"({"name":"review-pr"})",
                  },
              },
          .created_at = std::nullopt,
      },
      core::Message{
          .role = core::Role::tool,
          .blocks =
              {
                  core::ToolResultContent{
                      .tool_use_id = "skill-1",
                      .output = "release body",
                      .data_json = *release_activation,
                      .is_error = false,
                  },
                  core::ToolResultContent{
                      .tool_use_id = "skill-2",
                      .output = "review body",
                      .data_json = *review_activation,
                      .is_error = false,
                  },
              },
          .created_at = std::nullopt,
      },
  };
  const std::vector<skill::CatalogEntry> available{
      skill::CatalogEntry{
          .name = "release-note",
          .description = "Draft release notes.",
          .triggers = {},
          .model_hint = std::nullopt,
      },
      skill::CatalogEntry{
          .name = "review-pr",
          .description = "Review code changes.",
          .triggers = {},
          .model_hint = std::nullopt,
      },
  };

  const auto active = skill::resolve_active_skills(
      skill::ActivationPolicy{
          .deactivated_skill_names = {"release-note"},
      },
      std::span<const core::Message>{transcript},
      std::span<const skill::CatalogEntry>{available});
  const auto repeated = skill::resolve_active_skills(
      skill::ActivationPolicy{
          .deactivated_skill_names = {"release-note"},
      },
      std::span<const core::Message>{transcript},
      std::span<const skill::CatalogEntry>{available});

  REQUIRE(active.has_value());
  REQUIRE(repeated.has_value());
  REQUIRE(*active == std::vector<skill::ActiveSkill>{skill::ActiveSkill{.name = "review-pr"}});
  REQUIRE(*active == *repeated);
}

TEST_CASE("ActivationPolicy rejects invalid explicit deactivations", "[unit][skill][catalog]") {
  const std::vector<skill::CatalogEntry> available{
      skill::CatalogEntry{
          .name = "release-note",
          .description = "Draft release notes.",
          .triggers = {},
          .model_hint = std::nullopt,
      },
  };

  const auto duplicate = skill::resolve_active_skills(
      skill::ActivationPolicy{
          .deactivated_skill_names = {"release-note", "release-note"},
      },
      std::span<const core::Message>{},
      std::span<const skill::CatalogEntry>{available});
  REQUIRE_FALSE(duplicate.has_value());
  REQUIRE(duplicate.error().kind() == core::ErrorKind::invalid_argument);

  const auto blank = skill::resolve_active_skills(
      skill::ActivationPolicy{
          .deactivated_skill_names = {" "},
      },
      std::span<const core::Message>{},
      std::span<const skill::CatalogEntry>{available});
  REQUIRE_FALSE(blank.has_value());
  REQUIRE(blank.error().kind() == core::ErrorKind::invalid_argument);
}

TEST_CASE("ActivationPolicy expires transcript-derived active markers at explicit evaluation time",
          "[unit][skill][catalog]") {
  auto release_activation = skill::render_activation_data_json("release-note");
  auto review_activation = skill::render_activation_data_json("review-pr");
  REQUIRE(release_activation.has_value());
  REQUIRE(review_activation.has_value());
  const std::vector<core::Message> transcript{
      core::Message{
          .role = core::Role::assistant,
          .blocks =
              {
                  core::ToolUseContent{
                      .id = "skill-1",
                      .name = "SkillInvoke",
                      .input_json = R"({"name":"release-note"})",
                  },
                  core::ToolUseContent{
                      .id = "skill-2",
                      .name = "SkillInvoke",
                      .input_json = R"({"name":"review-pr"})",
                  },
              },
          .created_at = std::nullopt,
      },
      core::Message{
          .role = core::Role::tool,
          .blocks =
              {
                  core::ToolResultContent{
                      .tool_use_id = "skill-1",
                      .output = "release body",
                      .data_json = *release_activation,
                      .is_error = false,
                  },
                  core::ToolResultContent{
                      .tool_use_id = "skill-2",
                      .output = "review body",
                      .data_json = *review_activation,
                      .is_error = false,
                  },
              },
          .created_at = std::nullopt,
      },
  };
  const std::vector<skill::CatalogEntry> available{
      skill::CatalogEntry{
          .name = "release-note",
          .description = "Draft release notes.",
          .triggers = {},
          .model_hint = std::nullopt,
      },
      skill::CatalogEntry{
          .name = "review-pr",
          .description = "Review code changes.",
          .triggers = {},
          .model_hint = std::nullopt,
      },
  };
  const auto now = core::Time{core::Time::time_point{std::chrono::seconds{10}}};
  const auto past = core::Time{core::Time::time_point{std::chrono::seconds{5}}};
  const auto future = core::Time{core::Time::time_point{std::chrono::seconds{15}}};

  const auto active = skill::resolve_active_skills(
      skill::ActivationPolicy{
          .evaluation_time = now,
          .expirations =
              {
                  skill::SkillExpiration{.name = "release-note", .expires_at = past},
                  skill::SkillExpiration{.name = "review-pr", .expires_at = future},
              },
      },
      std::span<const core::Message>{transcript},
      std::span<const skill::CatalogEntry>{available});
  const auto repeated = skill::resolve_active_skills(
      skill::ActivationPolicy{
          .evaluation_time = now,
          .expirations =
              {
                  skill::SkillExpiration{.name = "release-note", .expires_at = past},
                  skill::SkillExpiration{.name = "review-pr", .expires_at = future},
              },
      },
      std::span<const core::Message>{transcript},
      std::span<const skill::CatalogEntry>{available});

  REQUIRE(active.has_value());
  REQUIRE(repeated.has_value());
  REQUIRE(*active == std::vector<skill::ActiveSkill>{skill::ActiveSkill{.name = "review-pr"}});
  REQUIRE(*active == *repeated);
}

TEST_CASE("ActivationPolicy rejects invalid explicit expirations", "[unit][skill][catalog]") {
  const std::vector<skill::CatalogEntry> available{
      skill::CatalogEntry{
          .name = "release-note",
          .description = "Draft release notes.",
          .triggers = {},
          .model_hint = std::nullopt,
      },
  };
  const auto now = core::Time{core::Time::time_point{std::chrono::seconds{10}}};

  const auto missing_time = skill::resolve_active_skills(
      skill::ActivationPolicy{
          .expirations = {skill::SkillExpiration{.name = "release-note", .expires_at = now}},
      },
      std::span<const core::Message>{},
      std::span<const skill::CatalogEntry>{available});
  REQUIRE_FALSE(missing_time.has_value());
  REQUIRE(missing_time.error().kind() == core::ErrorKind::invalid_argument);

  const auto duplicate = skill::resolve_active_skills(
      skill::ActivationPolicy{
          .evaluation_time = now,
          .expirations =
              {
                  skill::SkillExpiration{.name = "release-note", .expires_at = now},
                  skill::SkillExpiration{.name = "release-note", .expires_at = now},
              },
      },
      std::span<const core::Message>{},
      std::span<const skill::CatalogEntry>{available});
  REQUIRE_FALSE(duplicate.has_value());
  REQUIRE(duplicate.error().kind() == core::ErrorKind::invalid_argument);

  const auto blank = skill::resolve_active_skills(
      skill::ActivationPolicy{
          .evaluation_time = now,
          .expirations = {skill::SkillExpiration{.name = " ", .expires_at = now}},
      },
      std::span<const core::Message>{},
      std::span<const skill::CatalogEntry>{available});
  REQUIRE_FALSE(blank.has_value());
  REQUIRE(blank.error().kind() == core::ErrorKind::invalid_argument);
}

TEST_CASE("ActivationPolicy rejects duplicate available skill entries", "[unit][skill][catalog]") {
  const std::vector<skill::CatalogEntry> available{
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

  const auto active = skill::resolve_active_skills(skill::ActivationPolicy{},
                                                   std::span<const core::Message>{},
                                                   std::span<const skill::CatalogEntry>{available});

  REQUIRE_FALSE(active.has_value());
  REQUIRE(active.error().kind() == core::ErrorKind::invalid_argument);
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
