// tests/io/test_directory_authority.cpp — dirfd authority regressions.

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include <oran/io/directory_authority.hpp>
#include <oran/io/fingerprint.hpp>

namespace core = orangutan::core;
namespace io = orangutan::io;

namespace {

class TempDir {
public:
  explicit TempDir(std::string name)
      : path_{std::filesystem::temp_directory_path() /
              (std::move(name) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))} {
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

void write_direct(const std::filesystem::path& path, std::string_view contents) {
  std::ofstream output{path, std::ios::binary};
  output << contents;
}

[[nodiscard]] std::string read_handle(const io::ReadOnlyFile& file) {
  std::array<char, 64> buffer{};
  const auto count = ::read(file.native_handle(), buffer.data(), buffer.size());
  REQUIRE(count >= 0);
  return std::string{buffer.data(), static_cast<std::size_t>(count)};
}

}  // namespace

TEST_CASE("DirectoryAuthority retains the opened root across pathname replacement", "[unit][io][authority]") {
  TempDir temp{"oran-io-authority-root-replacement"};
  const auto workspace = temp.path() / "workspace";
  const auto moved_workspace = temp.path() / "workspace-moved";
  const auto outside = temp.path() / "outside";
  std::filesystem::create_directories(workspace / "nested");
  std::filesystem::create_directories(outside / "nested");
  write_direct(workspace / "nested" / "note.txt", "inside");
  write_direct(outside / "nested" / "note.txt", "outside");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());

  std::filesystem::rename(workspace, moved_workspace);
  std::filesystem::create_directory_symlink(outside, workspace);

  auto child = authority->open_directory(io::AnchoredPath{
      .relative_path = "nested",
      .symlink_policy = io::AnchoredSymlinkPolicy::allow_beneath,
  });
  REQUIRE(child.has_value());

  auto opened = child->open_file(io::AnchoredPath{
      .relative_path = "note.txt",
      .symlink_policy = io::AnchoredSymlinkPolicy::allow_beneath,
  });
  REQUIRE(opened.has_value());
  REQUIRE(read_handle(*opened) == "inside");
  auto fingerprint = io::compute_file_fingerprint(*opened);
  REQUIRE(fingerprint.has_value());
  CHECK(fingerprint->size_bytes == 6);

  std::ifstream redirected{workspace / "nested" / "note.txt", std::ios::binary};
  std::string redirected_text;
  redirected >> redirected_text;
  REQUIRE(redirected_text == "outside");
}

TEST_CASE("DirectoryAuthority rejects traversal and symlink escapes", "[unit][io][authority]") {
  TempDir temp{"oran-io-authority-confinement"};
  const auto workspace = temp.path() / "workspace";
  const auto outside = temp.path() / "outside";
  std::filesystem::create_directories(workspace);
  std::filesystem::create_directories(outside);
  write_direct(outside / "secret.txt", "secret");
  std::filesystem::create_directory_symlink(outside, workspace / "escape");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());

  auto traversal = authority->open_file(io::AnchoredPath{
      .relative_path = "../outside/secret.txt",
      .symlink_policy = io::AnchoredSymlinkPolicy::allow_beneath,
  });
  REQUIRE_FALSE(traversal.has_value());
  REQUIRE(traversal.error().kind() == core::ErrorKind::permission_denied);

  auto absolute = authority->open_file(io::AnchoredPath{
      .relative_path = (outside / "secret.txt").string(),
      .symlink_policy = io::AnchoredSymlinkPolicy::allow_beneath,
  });
  REQUIRE_FALSE(absolute.has_value());
  REQUIRE(absolute.error().kind() == core::ErrorKind::invalid_argument);

  auto symlink_escape = authority->open_file(io::AnchoredPath{
      .relative_path = "escape/secret.txt",
      .symlink_policy = io::AnchoredSymlinkPolicy::allow_beneath,
  });
  REQUIRE_FALSE(symlink_escape.has_value());
  REQUIRE(symlink_escape.error().kind() == core::ErrorKind::permission_denied);
}

TEST_CASE("DirectoryAuthority reject-all policy refuses an in-root symlink", "[unit][io][authority]") {
  TempDir temp{"oran-io-authority-symlink-policy"};
  const auto workspace = temp.path() / "workspace";
  std::filesystem::create_directories(workspace);
  write_direct(workspace / "real.txt", "inside");
  std::filesystem::create_symlink("real.txt", workspace / "link.txt");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());

  auto rejected = authority->open_file(io::AnchoredPath{
      .relative_path = "link.txt",
      .symlink_policy = io::AnchoredSymlinkPolicy::reject_all,
  });
  REQUIRE_FALSE(rejected.has_value());
  REQUIRE(rejected.error().kind() == core::ErrorKind::permission_denied);
}
