// tests/tool/test_output.cpp — structured Output envelope coverage.

#include <chrono>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <oran/tool/output.hpp>

using namespace std::chrono_literals;

namespace tool = orangutan::tool;

TEST_CASE("Output::text_only preserves v1 text shape", "[unit][tool][output]") {
  const auto output = tool::Output::text_only("hello");

  REQUIRE(output.text == "hello");
  REQUIRE_FALSE(output.data_json.has_value());
  REQUIRE(output.attachments.empty());
  REQUIRE(output.usage.empty());
  REQUIRE_FALSE(output.is_error);
}

TEST_CASE("Output::error marks an error envelope and may carry data_json", "[unit][tool][output]") {
  const auto output = tool::Output::error("bad input", std::string{R"({"kind":"invalid_argument"})"});

  REQUIRE(output.text == "bad input");
  REQUIRE(output.data_json == R"({"kind":"invalid_argument"})");
  REQUIRE(output.attachments.empty());
  REQUIRE(output.usage.empty());
  REQUIRE(output.is_error);
}

TEST_CASE("ToolUsage::empty reflects optional counters and flags", "[unit][tool][output]") {
  auto usage = tool::ToolUsage{};
  REQUIRE(usage.empty());

  usage.bytes_read = 4096;
  REQUIRE_FALSE(usage.empty());

  usage.bytes_read.reset();
  REQUIRE(usage.empty());

  usage.wall_time = 5ms;
  REQUIRE_FALSE(usage.empty());

  usage.wall_time.reset();
  usage.truncated = true;
  REQUIRE_FALSE(usage.empty());
}

TEST_CASE("Output carries serialized data and attachment metadata without parsing JSON", "[unit][tool][output]") {
  tool::Output output{
      .text = "matched 1 file",
      .data_json = std::string{R"({"matches":[{"path":"a.txt","line":1}]})"},
      .attachments =
          {
              tool::Attachment{
                  .file_path = "a.txt",
                  .mime_type = "text/plain",
                  .byte_size = 12,
                  .fingerprint = "v1:abc",
              },
          },
      .usage =
          tool::ToolUsage{
              .bytes_read = 12,
              .files_touched = 1,
              .match_count = 1,
          },
  };

  REQUIRE(output.data_json.has_value());
  REQUIRE(output.attachments.size() == 1);
  REQUIRE(output.attachments[0].file_path == "a.txt");
  REQUIRE(output.attachments[0].mime_type == "text/plain");
  REQUIRE(output.attachments[0].byte_size == 12);
  REQUIRE(output.attachments[0].fingerprint == "v1:abc");
  REQUIRE(output.usage.bytes_read == 12);
  REQUIRE(output.usage.files_touched == 1);
  REQUIRE(output.usage.match_count == 1);
  REQUIRE_FALSE(output.is_error);
}
