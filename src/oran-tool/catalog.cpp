// src/oran-tool/catalog.cpp — deterministic tool-catalog renderer.

#include <oran/tool/catalog.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <iterator>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <oran/core/bounded_cache.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/core/time.hpp>

namespace orangutan::tool {

namespace {

using json = ::nlohmann::json;

constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;
constexpr std::string_view kHashSeparator = "\x1F";

[[nodiscard]] std::uint64_t hash_append(std::uint64_t hash, std::string_view bytes) noexcept {
  for (const char c : bytes) {
    hash ^= static_cast<unsigned char>(c);
    hash *= kFnvPrime;
  }
  return hash;
}

[[nodiscard]] std::uint64_t hash_append_u64(std::uint64_t hash, std::uint64_t value) noexcept {
  std::array<char, 8> bytes{};
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<char>((value >> (i * 8)) & 0xFFu);
  }
  return hash_append(hash, std::string_view{bytes.data(), bytes.size()});
}

[[nodiscard]] std::string join_lines(const std::vector<std::string>& rows, std::string_view separator) {
  return rows | std::views::join_with(separator) | std::ranges::to<std::string>();
}

[[nodiscard]] std::vector<std::string_view> sorted_capability_names(const std::vector<core::Capability>& capabilities) {
  std::vector<std::string_view> names;
  names.reserve(capabilities.size());
  for (const auto capability : capabilities) {
    names.push_back(core::enum_name(capability));
  }
  std::ranges::sort(names);
  return names;
}

[[nodiscard]] std::string render_capabilities(const std::vector<core::Capability>& capabilities) {
  return sorted_capability_names(capabilities) | std::views::join_with(std::string_view{", "}) |
         std::ranges::to<std::string>();
}

[[nodiscard]] core::Result<std::string> canonical_schema(std::string_view tool_name, std::string_view schema_json) {
  try {
    auto parsed = json::parse(schema_json);
    if (!parsed.is_object()) {
      return std::unexpected(core::Error::invalid_argument("tool input_schema_json must be a JSON Schema object")
                                 .with("tool", std::string{tool_name})
                                 .with("schema_path", "$"));
    }
    return parsed.dump(2, ' ', false, json::error_handler_t::strict);
  } catch (const json::parse_error& e) {
    return std::unexpected(core::Error::invalid_argument("tool input_schema_json is not valid JSON")
                               .with("tool", std::string{tool_name})
                               .with("schema_path", "$")
                               .with("detail", e.what()));
  } catch (const json::exception& e) {
    return std::unexpected(core::Error::invalid_argument("tool input_schema_json cannot be rendered")
                               .with("tool", std::string{tool_name})
                               .with("schema_path", "$")
                               .with("detail", e.what()));
  } catch (const std::exception& e) {
    return std::unexpected(core::Error::invalid_argument("tool input_schema_json render failed")
                               .with("tool", std::string{tool_name})
                               .with("schema_path", "$")
                               .with("detail", e.what()));
  }
}

[[nodiscard]] std::string render_uncached_block(const core::ToolDef& def, std::string canonical_schema_json) {
  std::string block;
  block.reserve(def.name.size() + def.description.size() + canonical_schema_json.size() + 128);
  std::format_to(std::back_inserter(block), "Tool: {}\nDescription: {}\n", def.name, def.description);
  if (def.category.has_value() && !def.category->empty()) {
    std::format_to(std::back_inserter(block), "Category: {}\n", *def.category);
  }
  block.append("Capabilities: ");
  if (def.required_capabilities.empty()) {
    block.append("none");
  } else {
    block.append(render_capabilities(def.required_capabilities));
  }
  block.append("\nInput Schema:\n");
  block.append(canonical_schema_json);
  return block;
}

[[nodiscard]] std::string render_deferred_entry(const core::ToolDef& def) {
  return std::format("{} - {}", def.name, def.description);
}

struct CachedBlock {
  std::string fingerprint;
  std::string rendered;
};

struct BlockByteSize {
  [[nodiscard]] std::size_t operator()(const CachedBlock& value) const noexcept {
    return value.fingerprint.size() + value.rendered.size();
  }
};

[[nodiscard]] std::string tool_def_fingerprint(const core::ToolDef& def, std::uint32_t renderer_version) {
  std::string fingerprint;
  fingerprint.reserve(def.name.size() + def.description.size() + def.input_schema_json.size() + 64);
  fingerprint.append(std::to_string(renderer_version)).append(kHashSeparator);
  fingerprint.append(def.name).append(kHashSeparator);
  fingerprint.append(def.description).append(kHashSeparator);
  fingerprint.append(def.input_schema_json).append(kHashSeparator);
  for (const auto capability_name : sorted_capability_names(def.required_capabilities)) {
    fingerprint.append(capability_name).append(kHashSeparator);
  }
  if (def.category.has_value()) {
    fingerprint.append(*def.category);
  }
  return fingerprint;
}

}  // namespace

std::uint64_t tool_def_render_hash(const core::ToolDef& def, std::uint32_t renderer_version) noexcept {
  std::uint64_t hash = kFnvOffset;
  hash = hash_append_u64(hash, renderer_version);
  hash = hash_append(hash, kHashSeparator);
  hash = hash_append(hash, def.name);
  hash = hash_append(hash, kHashSeparator);
  hash = hash_append(hash, def.description);
  hash = hash_append(hash, kHashSeparator);
  hash = hash_append(hash, def.input_schema_json);
  hash = hash_append(hash, kHashSeparator);
  for (const auto capability : core::enum_values<core::Capability>()) {
    const auto occurrences = std::ranges::count(def.required_capabilities, capability);
    for (std::size_t i = 0; i < static_cast<std::size_t>(occurrences); ++i) {
      hash = hash_append(hash, core::enum_name(capability));
      hash = hash_append(hash, kHashSeparator);
    }
  }
  if (def.category.has_value()) {
    hash = hash_append(hash, *def.category);
  }
  return hash;
}

class CatalogRenderer::Impl {
public:
  explicit Impl(const ToolCatalogRenderOptions& options)
      : cache_enabled_{options.max_cached_blocks != 0},
        block_cache_{
            core::BoundedCache<std::uint64_t, CachedBlock, BlockByteSize>::Options{
                .max_entries = cache_enabled_ ? options.max_cached_blocks : 1,
                .max_bytes = 0,
                .ttl = std::chrono::seconds{0},
            },
            BlockByteSize{},
        } {}

  [[nodiscard]] CachedBlock* get(std::uint64_t key, core::Time now) {
    if (!cache_enabled_) {
      ++disabled_misses_;
      return nullptr;
    }
    return block_cache_.get(key, now);
  }

  void put(std::uint64_t key, CachedBlock value, core::Time now) {
    if (!cache_enabled_) {
      return;
    }
    block_cache_.put(key, std::move(value), now);
  }

  [[nodiscard]] ToolCatalogBlockCacheStats stats() const noexcept {
    const auto& stats = block_cache_.stats();
    return ToolCatalogBlockCacheStats{
        .hits = stats.hits,
        .misses = stats.misses + disabled_misses_,
        .evictions_lru = stats.evictions_lru,
        .evictions_ttl = stats.evictions_ttl,
        .evictions_bytes = stats.evictions_bytes,
        .rejected_oversize = stats.rejected_oversize,
        .current_entries = stats.current_entries,
        .current_bytes = stats.current_bytes,
    };
  }

private:
  bool cache_enabled_{true};
  std::uint64_t disabled_misses_{0};
  core::BoundedCache<std::uint64_t, CachedBlock, BlockByteSize> block_cache_;
};

CatalogRenderer::CatalogRenderer(ToolCatalogRenderOptions options)
    : options_{options}, impl_{std::make_unique<Impl>(options_)} {}

CatalogRenderer::~CatalogRenderer() = default;

CatalogRenderer::CatalogRenderer(CatalogRenderer&&) noexcept = default;

CatalogRenderer& CatalogRenderer::operator=(CatalogRenderer&&) noexcept = default;

core::Result<std::string> CatalogRenderer::render_tool_block(const core::ToolDef& def) {
  const auto key = tool_def_render_hash(def, options_.renderer_version);
  const auto fingerprint = tool_def_fingerprint(def, options_.renderer_version);
  const auto now = core::Time{};
  if (auto* cached = impl_->get(key, now); cached != nullptr && cached->fingerprint == fingerprint) {
    return cached->rendered;
  }

  auto schema = canonical_schema(def.name, def.input_schema_json);
  if (!schema) {
    return std::unexpected(std::move(schema).error());
  }

  auto rendered = render_uncached_block(def, std::move(*schema));
  impl_->put(key, CachedBlock{.fingerprint = fingerprint, .rendered = rendered}, now);
  return rendered;
}

core::Result<RenderedToolCatalog> CatalogRenderer::render_catalog(std::span<const core::ToolDef> defs) {
  std::vector<const core::ToolDef*> ordered;
  ordered.reserve(defs.size());
  for (const auto& def : defs) {
    ordered.push_back(&def);
  }
  std::ranges::sort(ordered, {}, [](const core::ToolDef* def) { return std::string_view{def->name}; });

  RenderedToolCatalog catalog;
  for (const auto* def : ordered) {
    if (def->deferred) {
      catalog.deferred_entries.push_back(render_deferred_entry(*def));
      continue;
    }
    auto block = render_tool_block(*def);
    if (!block) {
      return std::unexpected(std::move(block).error());
    }
    catalog.active_blocks.push_back(std::move(*block));
  }

  catalog.active_text = join_lines(catalog.active_blocks, "\n\n");
  catalog.deferred_text = join_lines(catalog.deferred_entries, "\n");
  return catalog;
}

ToolCatalogCacheStats CatalogRenderer::cache_stats() const noexcept {
  return ToolCatalogCacheStats{
      .renderer_version = options_.renderer_version,
      .blocks = impl_->stats(),
  };
}

}  // namespace orangutan::tool
