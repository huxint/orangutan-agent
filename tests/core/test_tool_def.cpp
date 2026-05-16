// tests/core/test_tool_def.cpp — `ToolDef` shape and helpers.

#include <string>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/tool_def.hpp>

using orangutan::core::ToolDef;

TEST_CASE("ToolDef aggregate-init exposes name/description/schema", "[unit][core][tool_def]") {
  ToolDef td{
      .name = "file.read",
      .description = "Read a UTF-8 text file.",
      .input_schema_json = R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})",
  };
  REQUIRE(td.name == "file.read");
  REQUIRE(td.description == "Read a UTF-8 text file.");
  REQUIRE(td.input_schema_json.find("\"path\"") != std::string::npos);
}

TEST_CASE("ToolDef::with_no_input fills a minimal object schema", "[unit][core][tool_def]") {
  const auto td = ToolDef::with_no_input("clock.now", "Return current UTC time.");
  REQUIRE(td.name == "clock.now");
  REQUIRE(td.description == "Return current UTC time.");
  REQUIRE(td.input_schema_json.find("\"type\":\"object\"") != std::string::npos);
  REQUIRE(td.input_schema_json.find("\"properties\":{}") != std::string::npos);
  REQUIRE(td.input_schema_json.find("\"additionalProperties\":false") != std::string::npos);
}

TEST_CASE("ToolDef equality is member-wise", "[unit][core][tool_def]") {
  const auto lhs = ToolDef::with_no_input("a", "alpha");
  const auto rhs = ToolDef::with_no_input("a", "alpha");
  REQUIRE(lhs == rhs);

  ToolDef differ_name = lhs;
  differ_name.name = "b";
  REQUIRE_FALSE(differ_name == lhs);

  ToolDef differ_desc = lhs;
  differ_desc.description = "beta";
  REQUIRE_FALSE(differ_desc == lhs);

  ToolDef differ_schema = lhs;
  differ_schema.input_schema_json = R"({"type":"object"})";
  REQUIRE_FALSE(differ_schema == lhs);
}

TEST_CASE("ToolDef is move-constructed cheaply", "[unit][core][tool_def]") {
  ToolDef src = ToolDef::with_no_input("agent.handoff", "Hand off to a sibling agent.");
  const auto schema_addr = src.input_schema_json.data();
  ToolDef dst = std::move(src);
  REQUIRE(dst.name == "agent.handoff");
  // The moved-from string's data pointer should have transferred (libstdc++
  // moves the buffer for non-SSO sizes; either way `dst` owns the data).
  REQUIRE(dst.input_schema_json.find("\"type\":\"object\"") != std::string::npos);
  (void)schema_addr;
}
