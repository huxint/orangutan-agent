#include <catch2/catch_test_macros.hpp>

#include <oran/agent/conversation.hpp>

namespace agent = orangutan::agent;
namespace core = orangutan::core;

TEST_CASE("conversation preparation preserves complete typed exchanges", "[unit][agent][context][core_boundary]") {
  const auto question = core::Message::user_text("Read the note");
  const auto call = core::Message{
      .role = core::Role::assistant,
      .blocks = {core::ThinkingContent{.thinking = "Read first", .signature = "signed"},
                 core::ToolUseContent{.id = "read-1", .name = "FileRead", .input_json = R"({"path":"note.txt"})"}},
      .created_at = {},
  };
  const auto result = core::Message{
      .role = core::Role::tool,
      .blocks = {core::ToolResultContent{.tool_use_id = "read-1", .output = "note body", .data_json = "{}"}},
      .created_at = {},
  };
  const auto answer = core::Message::assistant_text("The note is ready");

  auto prepared = agent::prepare_conversation(
      {result, core::Message::assistant_text("Orphaned answer"), question, call, result, answer},
      "Continue");

  CHECK(prepared.history_size == 4);
  CHECK(prepared.messages ==
        std::vector<core::Message>{question, call, result, answer, core::Message::user_text("Continue")});
}

TEST_CASE("conversation preparation starts a turn when no complete exchange remains",
          "[unit][agent][context][core_boundary]") {
  auto prepared = agent::prepare_conversation({core::Message::assistant_text("Orphaned answer")}, "Start here");

  CHECK(prepared.history_size == 0);
  CHECK(prepared.messages == std::vector<core::Message>{core::Message::user_text("Start here")});
}
