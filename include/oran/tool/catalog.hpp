// include/oran/tool/catalog.hpp — deterministic tool-catalog rendering.
//
// The future `oran-prompt` builder consumes tool catalog bytes, but the
// stable input for those bytes is already the `oran-tool` catalog snapshot.
// This renderer lives next to the registry so schema rendering, deferred-tool
// indexing, and bounded rendered-block caching stay close to the tool surface.
//
// JSON remains a private implementation detail: callers pass `core::ToolDef`
// values with opaque schema strings, and the `.cpp` canonicalises them with
// nlohmann/json before writing prompt-facing bytes.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <oran/core/result.hpp>
#include <oran/core/tool_def.hpp>

namespace orangutan::tool {

struct ToolCatalogRenderOptions {
  /// Bumped when the rendered block format changes. This is the catalog-block
  /// cache version from specs 0012/0016, not the prompt section version.
  std::uint32_t renderer_version{1};
  /// Max rendered full-schema blocks kept hot. `0` disables memoisation
  /// rather than creating an unbounded cache.
  std::size_t max_cached_blocks{256};
};

struct ToolCatalogBlockCacheStats {
  std::uint64_t hits{0};
  std::uint64_t misses{0};
  std::uint64_t evictions_lru{0};
  std::uint64_t evictions_ttl{0};
  std::uint64_t evictions_bytes{0};
  std::uint64_t rejected_oversize{0};
  std::size_t current_entries{0};
  std::size_t current_bytes{0};
};

struct ToolCatalogCacheStats {
  std::uint32_t renderer_version{1};
  ToolCatalogBlockCacheStats blocks{};
};

struct RenderedToolCatalog {
  /// Full-schema blocks for non-deferred tools, sorted by tool name.
  std::vector<std::string> active_blocks;
  /// Name+description rows for deferred tools, sorted by tool name.
  std::vector<std::string> deferred_entries;
  /// `active_blocks` joined with one blank line.
  std::string active_text;
  /// `deferred_entries` joined with newlines.
  std::string deferred_text;
};

/// Single-strand renderer. Like `core::BoundedCache`, this type does not hide
/// a mutex; prompt/agent code owns it on one strand or wraps it explicitly.
class CatalogRenderer {
public:
  explicit CatalogRenderer(ToolCatalogRenderOptions options = {});
  ~CatalogRenderer();

  CatalogRenderer(const CatalogRenderer&) = delete;
  CatalogRenderer& operator=(const CatalogRenderer&) = delete;
  CatalogRenderer(CatalogRenderer&&) noexcept;
  CatalogRenderer& operator=(CatalogRenderer&&) noexcept;

  [[nodiscard]] core::Result<std::string> render_tool_block(const core::ToolDef& def);
  [[nodiscard]] core::Result<RenderedToolCatalog> render_catalog(std::span<const core::ToolDef> defs);

  [[nodiscard]] ToolCatalogCacheStats cache_stats() const noexcept;
  [[nodiscard]] const ToolCatalogRenderOptions& options() const noexcept {
    return options_;
  }

private:
  class Impl;
  ToolCatalogRenderOptions options_{};
  std::unique_ptr<Impl> impl_;
};

/// Hash of the full-schema rendered block inputs plus renderer version. The
/// `ToolDef::deferred` placement bit is intentionally excluded because it
/// decides whether the block appears in section 2 or section 3; it does not
/// change the full-schema block bytes.
[[nodiscard]] std::uint64_t tool_def_render_hash(const core::ToolDef& def, std::uint32_t renderer_version) noexcept;

}  // namespace orangutan::tool
