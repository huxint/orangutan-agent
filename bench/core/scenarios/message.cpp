// bench/core/scenarios/message.cpp
//
// A-vs-B coverage for the `core::Content` variant traversal:
//
//   1. `core.content_visit_overloaded` : `std::visit(Overloaded{...}, c)` —
//                                        the project-preferred visitor style
//                                        from `docs/rules/code-style.md`.
//   2. `core.content_get_if_text`      : `std::get_if<TextContent>(&c)` —
//                                        the single-alternative fast path.
//   3. `core.message_walk_blocks`      : walks a 32-block mixed-alternative
//                                        message and accumulates total text
//                                        length. Reflects the "render this
//                                        turn" hot path used by the CLI and
//                                        future memory tier.
//
// Both A and B walk the same 32-block message; the comparison documents the
// cost of preferring `std::visit` over a single-alternative shortcut so
// future hot paths can pick with eyes open.

#include <nanobench.h>

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

#include <oran/core/content.hpp>
#include <oran/core/message.hpp>
#include <oran/core/role.hpp>

namespace orangutan::bench {

namespace {

using core::Content;
using core::Message;
using core::Role;
using core::TextContent;
using core::ThinkingContent;
using core::ToolResultContent;
using core::ToolUseContent;

struct TextLengthVisitor {
  std::size_t operator()(const TextContent& t) const noexcept {
    return t.text.size();
  }
  std::size_t operator()(const ThinkingContent& t) const noexcept {
    return t.thinking.size();
  }
  std::size_t operator()(const ToolUseContent&) const noexcept {
    return 0;
  }
  std::size_t operator()(const ToolResultContent& r) const noexcept {
    return r.output.size();
  }
};

[[nodiscard]] Message make_mixed_message(std::size_t block_count) {
  Message m;
  m.role = Role::assistant;
  m.blocks.reserve(block_count);
  for (std::size_t i = 0; i < block_count; ++i) {
    switch (i % 4) {
      case 0:
        m.blocks.emplace_back(TextContent{.text = "lorem ipsum dolor sit amet"});
        break;
      case 1:
        m.blocks.emplace_back(ThinkingContent{
            .thinking = "consider the problem from another angle",
            .signature = std::nullopt,
        });
        break;
      case 2:
        m.blocks.emplace_back(ToolUseContent{
            .id = "tool-" + std::to_string(i),
            .name = "file.read",
            .input_json = R"({"path":"README.md"})",
        });
        break;
      default:
        m.blocks.emplace_back(ToolResultContent{
            .tool_use_id = "tool-" + std::to_string(i - 1),
            .output = "ok",
            .data_json = std::nullopt,
            .is_error = false,
        });
        break;
    }
  }
  return m;
}

[[gnu::noinline]] std::size_t walk_via_visit(const Message& m) {
  std::size_t total = 0;
  for (const auto& c : m.blocks) {
    total += std::visit(TextLengthVisitor{}, c);
  }
  return total;
}

[[gnu::noinline]] std::size_t walk_via_get_if(const Message& m) {
  std::size_t total = 0;
  for (const auto& c : m.blocks) {
    if (const auto* t = std::get_if<TextContent>(&c)) {
      total += t->text.size();
    }
  }
  return total;
}

}  // namespace

void register_message_scenarios(ankerl::nanobench::Bench& bench) {
  const Message msg = make_mixed_message(32);

  bench.run("core.content_visit_overloaded", [&] {
    auto n = walk_via_visit(msg);
    ankerl::nanobench::doNotOptimizeAway(n);
  });
  bench.run("core.content_get_if_text", [&] {
    auto n = walk_via_get_if(msg);
    ankerl::nanobench::doNotOptimizeAway(n);
  });
  bench.run("core.message_walk_blocks", [&] {
    auto n = walk_via_visit(msg);
    ankerl::nanobench::doNotOptimizeAway(n);
  });
}

}  // namespace orangutan::bench
