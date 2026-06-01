// include/oran/skill/loader.hpp - markdown skill loader snapshot API.
//
// Skills are repository-local markdown templates with a small frontmatter
// metadata block. The loader owns the first filesystem snapshot shape for
// `<workspace>/.orangutan/skills/*.md`; bootstrap's `skill.invoke` callback
// consumes the same `SkillDocument` body snapshot. Hot-reload remains a later
// consumer.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <asio/any_io_executor.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/skill/catalog.hpp>

namespace orangutan::skill {

struct SkillMetadata {
  std::string name;
  std::string description;
  std::vector<std::string> triggers;
  std::optional<std::string> inputs_schema;
  std::optional<std::string> model_hint;

  friend bool operator==(const SkillMetadata&, const SkillMetadata&) = default;
};

struct SkillDocument {
  SkillMetadata metadata;
  std::string body;
  std::string source_path;

  friend bool operator==(const SkillDocument&, const SkillDocument&) = default;
};

struct LoaderOptions {
  /// Per-skill markdown body cap. The default matches spec 0009's 4 KiB
  /// prompt-bloat guard. The frontmatter has a separate bound so metadata
  /// cannot hide an unbounded file read.
  std::uintmax_t max_body_bytes{4U * 1024U};
  std::uintmax_t max_frontmatter_bytes{8U * 1024U};
  std::size_t max_directory_entries{256U};

  friend bool operator==(const LoaderOptions&, const LoaderOptions&) = default;
};

[[nodiscard]] CatalogEntry catalog_entry_from(const SkillDocument& document);
[[nodiscard]] std::vector<CatalogEntry> catalog_entries_from(std::span<const SkillDocument> documents);

class Loader {
public:
  explicit Loader(asio::any_io_executor executor, LoaderOptions options = {});

  [[nodiscard]] async::Awaitable<core::Result<SkillDocument>> load_file(std::string path) const;
  [[nodiscard]] async::Awaitable<core::Result<std::vector<SkillDocument>>> load_directory(std::string directory) const;
  [[nodiscard]] async::Awaitable<core::Result<RenderedCatalog>> load_catalog(std::string directory) const;

private:
  asio::any_io_executor executor_{};
  LoaderOptions options_{};
};

}  // namespace orangutan::skill
