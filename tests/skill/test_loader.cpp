// tests/skill/test_loader.cpp - markdown skill loader coverage.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/core/error.hpp>
#include <oran/skill.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace skill = orangutan::skill;
namespace test = orangutan::tests;

namespace {

class TempDir {
public:
  explicit TempDir(std::string name)
      : path_(std::filesystem::temp_directory_path() /
              (std::move(name) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
    std::filesystem::create_directories(path_);
  }

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  auto output = std::ofstream{path, std::ios::binary};
  output << contents;
}

}  // namespace

TEST_CASE("Loader parses markdown skill metadata and body", "[unit][skill][loader]") {
  TempDir temp{"oran-skill-loader-file"};
  const auto file = temp.path() / "release-note.md";
  write_file(file,
             "---\n"
             "name: release-note\n"
             "description: Draft release notes from completed changes.\n"
             "triggers: release notes, changelog\n"
             "inputs: {\"type\":\"object\"}\n"
             "model_hint: keep output concise\n"
             "---\n"
             "Use the completed history entries to draft concise release notes.\n");

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    auto loaded = co_await skill::Loader{io.get_executor()}.load_file(file.string());

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->metadata.name == "release-note");
    REQUIRE(loaded->metadata.description == "Draft release notes from completed changes.");
    REQUIRE(loaded->metadata.triggers == std::vector<std::string>{"release notes", "changelog"});
    REQUIRE(loaded->metadata.inputs_schema == std::string{"{\"type\":\"object\"}"});
    REQUIRE(loaded->metadata.model_hint == std::string{"keep output concise"});
    REQUIRE(loaded->body.contains("completed history entries"));
    REQUIRE(loaded->source_path == file.string());
  });
}

TEST_CASE("Loader rejects malformed or oversized skill files", "[unit][skill][loader]") {
  TempDir temp{"oran-skill-loader-invalid"};

  SECTION("missing frontmatter") {
    const auto file = temp.path() / "missing-frontmatter.md";
    write_file(file, "body only\n");

    test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
      auto loaded = co_await skill::Loader{io.get_executor()}.load_file(file.string());

      REQUIRE_FALSE(loaded.has_value());
      REQUIRE(loaded.error().kind() == core::ErrorKind::parsing);
    });
  }

  SECTION("missing required metadata") {
    const auto file = temp.path() / "missing-name.md";
    write_file(file,
               "---\n"
               "description: Missing a name.\n"
               "---\n"
               "Body.\n");

    test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
      auto loaded = co_await skill::Loader{io.get_executor()}.load_file(file.string());

      REQUIRE_FALSE(loaded.has_value());
      REQUIRE(loaded.error().kind() == core::ErrorKind::invalid_argument);
    });
  }

  SECTION("unknown metadata field") {
    const auto file = temp.path() / "unknown-field.md";
    write_file(file,
               "---\n"
               "name: review-pr\n"
               "description: Review a pull request.\n"
               "surprise: no\n"
               "---\n"
               "Body.\n");

    test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
      auto loaded = co_await skill::Loader{io.get_executor()}.load_file(file.string());

      REQUIRE_FALSE(loaded.has_value());
      REQUIRE(loaded.error().kind() == core::ErrorKind::invalid_argument);
    });
  }

  SECTION("frontmatter delimiter must be standalone") {
    const auto file = temp.path() / "bad-delimiter.md";
    write_file(file,
               "---\n"
               "name: release-note\n"
               "description: Draft release notes.\n"
               "--- trailing\n"
               "Body.\n");

    test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
      auto loaded = co_await skill::Loader{io.get_executor()}.load_file(file.string());

      REQUIRE_FALSE(loaded.has_value());
      REQUIRE(loaded.error().kind() == core::ErrorKind::parsing);
    });
  }

  SECTION("body size cap") {
    const auto file = temp.path() / "oversize.md";
    auto contents = std::string{"---\n"
                                "name: small\n"
                                "description: Too much body.\n"
                                "---\n"};
    contents.append(17, 'x');
    write_file(file, contents);

    test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
      auto loaded = co_await skill::Loader{io.get_executor(), skill::LoaderOptions{.max_body_bytes = 16}}.load_file(
          file.string());

      REQUIRE_FALSE(loaded.has_value());
      REQUIRE(loaded.error().kind() == core::ErrorKind::invalid_argument);
    });
  }
}

TEST_CASE("Loader snapshots a skill directory into deterministic catalog bytes", "[unit][skill][loader]") {
  TempDir temp{"oran-skill-loader-directory"};
  const auto dir = temp.path() / "skills";
  write_file(dir / "zeta.md",
             "---\n"
             "name: zeta\n"
             "description: Last alphabetically.\n"
             "triggers: final\n"
             "---\n"
             "Zeta body.\n");
  write_file(dir / "alpha.md",
             "---\n"
             "name: alpha\n"
             "description: First alphabetically.\n"
             "triggers: start, first\n"
             "---\n"
             "Alpha body.\n");
  write_file(dir / "README.txt", "ignored\n");

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    const auto loader = skill::Loader{io.get_executor()};
    auto documents = co_await loader.load_directory(dir.string());
    auto catalog = co_await loader.load_catalog(dir.string());

    REQUIRE(documents.has_value());
    REQUIRE(documents->size() == 2);
    REQUIRE((*documents)[0].metadata.name == "alpha");
    REQUIRE((*documents)[1].metadata.name == "zeta");

    REQUIRE(catalog.has_value());
    REQUIRE(catalog->section_text.starts_with("Skill: alpha\n"));
    REQUIRE(catalog->section_text.contains("Skill: zeta"));
    REQUIRE_FALSE(catalog->section_text.contains("Alpha body"));
  });
}

TEST_CASE("Loader treats a missing skills directory as an empty snapshot", "[unit][skill][loader]") {
  TempDir temp{"oran-skill-loader-missing"};
  const auto missing = temp.path() / "missing";

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    auto documents = co_await skill::Loader{io.get_executor()}.load_directory(missing.string());

    REQUIRE(documents.has_value());
    REQUIRE(documents->empty());
  });
}
