// bench/tool/scenarios/file_search.cpp
//
// A-vs-B comparisons for `FileSearch`:
//
//   1. `single_file_one_match` vs. `recursive_dir_many_matches` — full dispatch
//      on a single-file path vs. a directory-rooted walk over a 4-file / 14-line
//      tree. Both share the fixed dispatch costs (permission eval + libsodium
//      SHA-256 of the input + audit record + JSON parse + executor hop + read
//      of the matched file's contents); the contrast surfaces the
//      `recursive_directory_iterator` walk + per-file open/read overhead the
//      agent loop pays when a tool call reaches for a tree rather than a single
//      file. The baseline a future memory-mapped scan or parallel walker
//      optimization would have to beat.
//
//   2. `literal_match_1kib` vs. `regex_match_1kib` — full dispatch over the
//      same ~1 KiB seed file (32 lines × 32 bytes, one match in the middle).
//      The literal path uses `std::string_view::contains`; the regex path
//      routes the same pattern through `permission::InputPattern` and each
//      line through `re2::RE2::PartialMatch`. Slice 51 adds a bounded
//      compiled-regex cache, so repeated iterations with the same pattern
//      mostly measure the steady-state cached regex path. Use a unique-pattern
//      scenario if cold compile cost needs a fresh number.

#include <nanobench.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/capability.hpp>
#include <oran/core/result.hpp>
#include <oran/permission/audit.hpp>
#include <oran/permission/rule_set.hpp>
#include <oran/tool/builtins.hpp>
#include <oran/tool/registry.hpp>

namespace orangutan::bench {

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace permission = orangutan::permission;
namespace tool = orangutan::tool;

namespace {

class TempTree {
public:
  TempTree()
      : root_(
            std::filesystem::temp_directory_path() /
            ("oran-tool-bench-search-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
    std::filesystem::create_directories(root_);
  }

  ~TempTree() {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  TempTree(const TempTree&) = delete;
  TempTree& operator=(const TempTree&) = delete;

  void write(const std::filesystem::path& relative, std::string_view contents) const {
    const auto target = root_ / relative;
    std::filesystem::create_directories(target.parent_path());
    std::ofstream out{target, std::ios::binary | std::ios::trunc};
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  }

  [[nodiscard]] std::string root_string() const {
    return root_.string();
  }

  [[nodiscard]] std::string child_string(const std::filesystem::path& relative) const {
    return (root_ / relative).string();
  }

private:
  std::filesystem::path root_;
};

[[gnu::noinline]] std::size_t
run_dispatch(asio::io_context& io, tool::Registry& registry, tool::DispatchContext& ctx, const std::string& input) {
  std::size_t bytes = 0;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        auto result = co_await registry.dispatch(tool::kFileSearchName, input, ctx);
        if (!result) {
          std::abort();
        }
        bytes += result->text.size();
        co_return;
      },
      asio::detached);
  io.run();
  io.restart();
  return bytes;
}

[[nodiscard]] std::string make_seed_1kib() {
  // 32 lines × 32 bytes = 1024 bytes. Line 16 carries the unique marker so the
  // search returns exactly one match; the surrounding lines are uniform filler
  // so the regex path's per-line cost dominates over any cache-locality
  // artefacts from short or jagged inputs.
  std::string contents;
  contents.reserve(1024U);
  for (std::size_t i = 0; i < 32U; ++i) {
    if (i == 16U) {
      contents.append("error code 42 marker line       \n");
    } else {
      contents.append("plain filler line of padding ok\n");
    }
  }
  return contents;
}

}  // namespace

void register_tool_file_search(ankerl::nanobench::Bench& bench) {
  tool::Registry registry;
  if (!tool::register_file_search(registry)) {
    std::abort();
  }

  permission::RuleSet rules;
  rules.add(permission::Rule{
      .verdict = permission::Verdict::allow,
      .tool_pattern = std::string{tool::kFileSearchName},
      .capability = core::Capability::read_file,
  });
  permission::RecordingAuditSink sink;

  asio::io_context io;
  tool::DispatchContext ctx{
      .executor = io.get_executor(),
      .mode = permission::Mode::strict,
      .rules = rules,
      .audit = sink,
      .scope_key = "scope-A",
      .agent_key = "bencher",
      .identity = "operator-1",
  };

  TempTree tree;
  // Single-file fixture: ~14 lines, one literal match.
  tree.write("solo.txt", "line one\nline two\nNEEDLE here\nline four\nline five\nline six\n");
  // Directory fixture: 4 files scattered across the tree, 5 NEEDLE matches.
  tree.write("a.txt", "NEEDLE on line one\nfiller\n");
  tree.write("sub/b.txt", "filler\nNEEDLE on line two\n");
  tree.write("sub/deep/c.txt", "NEEDLE\nfiller\nNEEDLE again\n");
  tree.write("notes.md", "NEEDLE in markdown\n");
  // Regex A/B fixture: 1 KiB seed with one match. The same physical file
  // serves both literal and regex scenarios so the disk-read cost is
  // identical between the two.
  tree.write("seed_1kib.txt", make_seed_1kib());

  const auto solo_path = tree.child_string("solo.txt");
  const auto seed_path = tree.child_string("seed_1kib.txt");
  const std::string solo_input = R"({"path":")" + solo_path + R"(","pattern":"NEEDLE"})";
  const std::string tree_input = R"({"path":")" + tree.root_string() + R"(","pattern":"NEEDLE"})";
  const std::string literal_input = R"({"path":")" + seed_path + R"(","pattern":"error code 42 marker"})";
  const std::string regex_input = R"({"path":")" + seed_path + R"(","pattern":"error code \\d+ marker","regex":true})";

  bench.run("file_search.single_file_one_match", [&] {
    sink.clear();
    const auto bytes = run_dispatch(io, registry, ctx, solo_input);
    ankerl::nanobench::doNotOptimizeAway(bytes);
  });
  bench.run("file_search.recursive_dir_many_matches", [&] {
    sink.clear();
    const auto bytes = run_dispatch(io, registry, ctx, tree_input);
    ankerl::nanobench::doNotOptimizeAway(bytes);
  });
  bench.run("file_search.literal_match_1kib", [&] {
    sink.clear();
    const auto bytes = run_dispatch(io, registry, ctx, literal_input);
    ankerl::nanobench::doNotOptimizeAway(bytes);
  });
  bench.run("file_search.regex_match_1kib", [&] {
    sink.clear();
    const auto bytes = run_dispatch(io, registry, ctx, regex_input);
    ankerl::nanobench::doNotOptimizeAway(bytes);
  });
}

}  // namespace orangutan::bench
