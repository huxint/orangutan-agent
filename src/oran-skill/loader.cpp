// src/oran-skill/loader.cpp - markdown skill loader implementation.

#include <oran/skill/loader.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <oran/core/error.hpp>
#include <oran/io.hpp>

namespace orangutan::skill {
namespace {

constexpr std::string_view kFrontmatterDelimiter = "---";
constexpr std::string_view kMarkdownExtension = ".md";

[[nodiscard]] bool is_blank(std::string_view value) noexcept {
  return std::ranges::all_of(value, [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; });
}

[[nodiscard]] std::string_view trim(std::string_view value) noexcept {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
    value.remove_suffix(1);
  }
  return value;
}

[[nodiscard]] bool contains_line_break(std::string_view value) noexcept {
  return value.contains('\n') || value.contains('\r');
}

[[nodiscard]] std::string_view strip_trailing_cr(std::string_view value) noexcept {
  if (!value.empty() && value.back() == '\r') {
    value.remove_suffix(1);
  }
  return value;
}

[[nodiscard]] core::Error parse_error(std::string message, std::string_view path) {
  return core::Error::parsing(std::move(message)).with("path", std::string{path});
}

[[nodiscard]] core::Error invalid_metadata(std::string message, std::string_view path, std::string_view field) {
  return core::Error::invalid_argument(std::move(message))
      .with("path", std::string{path})
      .with("field", std::string{field});
}

[[nodiscard]] core::Result<std::string_view>
require_single_line(std::string_view key, std::string_view value, std::string_view path) {
  const auto trimmed = trim(value);
  if (is_blank(trimmed)) {
    return std::unexpected(invalid_metadata("skill metadata field must not be empty", path, key));
  }
  if (contains_line_break(trimmed)) {
    return std::unexpected(invalid_metadata("skill metadata field must be single-line", path, key));
  }
  return trimmed;
}

[[nodiscard]] core::Result<std::vector<std::string>>
parse_triggers(std::string_view value, std::string_view path, std::string_view field) {
  const auto trimmed = trim(value);
  if (is_blank(trimmed)) {
    return {};
  }

  std::vector<std::string> triggers;
  std::size_t start = 0;
  while (start <= trimmed.size()) {
    const auto comma = trimmed.find(',', start);
    const auto end = comma == std::string_view::npos ? trimmed.size() : comma;
    const auto item = trim(trimmed.substr(start, end - start));
    if (is_blank(item)) {
      return std::unexpected(invalid_metadata("skill trigger must not be empty", path, field));
    }
    triggers.emplace_back(item);
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
  return triggers;
}

[[nodiscard]] core::Result<void>
apply_metadata_line(SkillMetadata& metadata, std::string_view line, std::string_view path) {
  const auto separator = line.find(':');
  if (separator == std::string_view::npos) {
    return std::unexpected(parse_error("skill frontmatter line must be key: value", path));
  }

  const auto key = trim(line.substr(0, separator));
  const auto raw_value = line.substr(separator + 1);
  if (key == "name") {
    auto value = require_single_line(key, raw_value, path);
    if (!value) {
      return std::unexpected(std::move(value).error());
    }
    metadata.name = *value;
    return {};
  }
  if (key == "description") {
    auto value = require_single_line(key, raw_value, path);
    if (!value) {
      return std::unexpected(std::move(value).error());
    }
    metadata.description = *value;
    return {};
  }
  if (key == "triggers") {
    auto triggers = parse_triggers(raw_value, path, key);
    if (!triggers) {
      return std::unexpected(std::move(triggers).error());
    }
    metadata.triggers = std::move(*triggers);
    return {};
  }
  if (key == "inputs") {
    auto value = require_single_line(key, raw_value, path);
    if (!value) {
      return std::unexpected(std::move(value).error());
    }
    metadata.inputs_schema = std::string{*value};
    return {};
  }
  if (key == "model_hint") {
    auto value = require_single_line(key, raw_value, path);
    if (!value) {
      return std::unexpected(std::move(value).error());
    }
    metadata.model_hint = std::string{*value};
    return {};
  }

  return std::unexpected(invalid_metadata("unknown skill metadata field", path, key));
}

[[nodiscard]] core::Result<SkillMetadata> parse_metadata(std::string_view frontmatter, std::string_view path) {
  auto metadata = SkillMetadata{};
  std::size_t line_start = 0;
  while (line_start <= frontmatter.size()) {
    const auto newline = frontmatter.find('\n', line_start);
    const auto line_end = newline == std::string_view::npos ? frontmatter.size() : newline;
    const auto line = trim(frontmatter.substr(line_start, line_end - line_start));
    if (!line.empty()) {
      if (auto applied = apply_metadata_line(metadata, line, path); !applied) {
        return std::unexpected(std::move(applied).error());
      }
    }
    if (newline == std::string_view::npos) {
      break;
    }
    line_start = newline + 1;
  }

  if (metadata.name.empty()) {
    return std::unexpected(invalid_metadata("skill metadata is missing required field", path, "name"));
  }
  if (metadata.description.empty()) {
    return std::unexpected(invalid_metadata("skill metadata is missing required field", path, "description"));
  }
  return metadata;
}

[[nodiscard]] core::Result<SkillDocument>
parse_document(std::string text, std::string source_path, const LoaderOptions& options) {
  const auto opening_end = text.find('\n');
  if (opening_end == std::string::npos) {
    return std::unexpected(parse_error("skill markdown must start with YAML frontmatter", source_path));
  }
  if (strip_trailing_cr(std::string_view{text}.substr(0, opening_end)) != kFrontmatterDelimiter) {
    return std::unexpected(parse_error("skill frontmatter opening delimiter must be on its own line", source_path));
  }

  const auto frontmatter_start = opening_end + 1;
  auto closing_offset = std::string::npos;
  auto closing_line_end = std::string::npos;
  auto line_start = frontmatter_start;
  while (line_start <= text.size()) {
    const auto newline = text.find('\n', line_start);
    const auto line_end = newline == std::string::npos ? text.size() : newline;
    if (strip_trailing_cr(std::string_view{text}.substr(line_start, line_end - line_start)) == kFrontmatterDelimiter) {
      closing_offset = line_start;
      closing_line_end = line_end;
      break;
    }
    if (newline == std::string::npos) {
      break;
    }
    line_start = newline + 1;
  }
  if (closing_offset == std::string::npos) {
    return std::unexpected(parse_error("skill markdown is missing closing frontmatter delimiter", source_path));
  }
  const auto frontmatter = std::string_view{text}.substr(frontmatter_start, closing_offset - frontmatter_start);
  if (frontmatter.size() > options.max_frontmatter_bytes) {
    return std::unexpected(core::Error::invalid_argument("skill frontmatter exceeds size limit")
                               .with("path", source_path)
                               .with("size", std::to_string(frontmatter.size()))
                               .with("max_bytes", std::to_string(options.max_frontmatter_bytes)));
  }

  auto body_start = closing_line_end;
  if (body_start < text.size() && text[body_start] == '\n') {
    ++body_start;
  }

  std::string body = text.substr(body_start);
  if (body.size() > options.max_body_bytes) {
    return std::unexpected(core::Error::invalid_argument("skill body exceeds size limit")
                               .with("path", source_path)
                               .with("size", std::to_string(body.size()))
                               .with("max_bytes", std::to_string(options.max_body_bytes)));
  }
  if (is_blank(body)) {
    return std::unexpected(core::Error::invalid_argument("skill body must not be empty").with("path", source_path));
  }

  auto metadata = parse_metadata(frontmatter, source_path);
  if (!metadata) {
    return std::unexpected(std::move(metadata).error());
  }

  return SkillDocument{
      .metadata = std::move(*metadata),
      .body = std::move(body),
      .source_path = std::move(source_path),
  };
}

[[nodiscard]] bool is_markdown_file(const io::DirectoryEntry& entry) {
  return entry.kind == io::DirectoryEntryKind::regular_file && entry.name.ends_with(kMarkdownExtension);
}

}  // namespace

CatalogEntry catalog_entry_from(const SkillDocument& document) {
  return CatalogEntry{
      .name = document.metadata.name,
      .description = document.metadata.description,
      .triggers = document.metadata.triggers,
      .model_hint = document.metadata.model_hint,
  };
}

std::vector<CatalogEntry> catalog_entries_from(std::span<const SkillDocument> documents) {
  std::vector<CatalogEntry> entries;
  entries.reserve(documents.size());
  for (const auto& document : documents) {
    entries.push_back(catalog_entry_from(document));
  }
  return entries;
}

Loader::Loader(asio::any_io_executor executor, LoaderOptions options)
    : executor_{std::move(executor)}, options_{options} {}

async::Awaitable<core::Result<SkillDocument>> Loader::load_file(std::string path) const {
  if (!executor_) {
    co_return std::unexpected(core::Error::invalid_argument("skill loader requires an executor"));
  }
  const auto read_limit = options_.max_frontmatter_bytes + options_.max_body_bytes + 32U;
  auto text = co_await io::read_text_file(executor_, path, io::ReadTextOptions{.max_bytes = read_limit});
  if (!text) {
    co_return std::unexpected(std::move(text).error());
  }
  co_return parse_document(std::move(*text), std::move(path), options_);
}

async::Awaitable<core::Result<std::vector<SkillDocument>>> Loader::load_directory(std::string directory) const {
  if (!executor_) {
    co_return std::unexpected(core::Error::invalid_argument("skill loader requires an executor"));
  }
  auto listed = co_await io::list_directory(
      executor_,
      directory,
      io::ListDirectoryOptions{.include_hidden = false, .max_entries = options_.max_directory_entries});
  if (!listed) {
    if (listed.error().kind() == core::ErrorKind::not_found) {
      co_return std::vector<SkillDocument>{};
    }
    co_return std::unexpected(std::move(listed).error());
  }

  std::vector<io::DirectoryEntry> candidates;
  candidates.reserve(listed->size());
  std::ranges::copy_if(*listed, std::back_inserter(candidates), is_markdown_file);
  std::ranges::sort(candidates, {}, &io::DirectoryEntry::name);

  std::vector<SkillDocument> documents;
  documents.reserve(candidates.size());
  for (const auto& entry : candidates) {
    auto loaded = co_await load_file(entry.path);
    if (!loaded) {
      co_return std::unexpected(std::move(loaded).error());
    }
    documents.push_back(std::move(*loaded));
  }
  co_return documents;
}

async::Awaitable<core::Result<RenderedCatalog>> Loader::load_catalog(std::string directory) const {
  auto documents = co_await load_directory(std::move(directory));
  if (!documents) {
    co_return std::unexpected(std::move(documents).error());
  }
  auto entries = catalog_entries_from(std::span<const SkillDocument>{*documents});
  co_return render_catalog(entries);
}

}  // namespace orangutan::skill
