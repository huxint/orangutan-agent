// include/oran/skill/loader.hpp - markdown skill loader snapshot API.
//
// Skills are repository-local markdown templates with a small frontmatter
// metadata block. The loader owns filesystem snapshots for
// `<workspace>/.orangutan/skills/*.md`; bootstrap's `skill.invoke` callback
// consumes the same `SkillDocument` body snapshot. The workspace snapshot owner
// adds watcher-backed refresh without moving skill bodies into prompt bytes.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
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

struct WorkspaceSkillSnapshotStats {
  std::uint64_t loads{0};
  std::uint64_t watcher_events{0};
  std::uint64_t watcher_invalidations{0};
  bool loaded{false};
  bool watcher_active{false};

  friend bool operator==(const WorkspaceSkillSnapshotStats&, const WorkspaceSkillSnapshotStats&) = default;
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

/// Workspace skill snapshot with watcher-backed refresh.
///
/// `refresh()` loads the directory on first use, then drains any pending
/// filesystem watcher events and reloads the rendered catalog plus invocation
/// document snapshot before the caller starts the next prompt turn. The
/// document vector and catalog therefore remain coherent for a whole turn.
class WorkspaceSkillSnapshot {
public:
  explicit WorkspaceSkillSnapshot(asio::any_io_executor executor, std::string directory, LoaderOptions options = {});
  ~WorkspaceSkillSnapshot();

  WorkspaceSkillSnapshot(const WorkspaceSkillSnapshot&) = delete;
  WorkspaceSkillSnapshot& operator=(const WorkspaceSkillSnapshot&) = delete;
  WorkspaceSkillSnapshot(WorkspaceSkillSnapshot&&) noexcept;
  WorkspaceSkillSnapshot& operator=(WorkspaceSkillSnapshot&&) noexcept;

  [[nodiscard]] async::Awaitable<core::Result<void>> refresh();
  [[nodiscard]] const std::vector<SkillDocument>& documents() const noexcept;
  [[nodiscard]] const RenderedCatalog& catalog() const noexcept;
  [[nodiscard]] WorkspaceSkillSnapshotStats stats() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace orangutan::skill
