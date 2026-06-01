// src/oran-skill/loader.cpp - markdown skill loader implementation.

#include <oran/skill/loader.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sys/inotify.h>
#include <unistd.h>
#endif

#include <oran/core/error.hpp>
#include <oran/io.hpp>

namespace orangutan::skill {
namespace {

constexpr std::string_view kFrontmatterDelimiter = "---";
constexpr std::string_view kMarkdownExtension = ".md";

#if defined(__linux__)
constexpr std::uint32_t kSkillWatchMask = IN_CLOSE_WRITE | IN_MODIFY | IN_ATTRIB | IN_DELETE | IN_MOVED_FROM |
                                          IN_MOVED_TO | IN_CREATE | IN_DELETE_SELF | IN_MOVE_SELF;
constexpr std::size_t kSkillWatchBufferBytes = 16U * 1024U;
#endif

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

struct SkillFileSignature {
  std::string name;
  std::uintmax_t size_bytes{0};
  std::int64_t mtime_ns{0};
  std::uint64_t content_hash{0};

  friend bool operator==(const SkillFileSignature&, const SkillFileSignature&) = default;
};

[[nodiscard]] core::Error
signature_filesystem_error(std::string message, std::string_view path, const std::error_code& ec) {
  if (ec == std::errc::permission_denied) {
    return core::Error::permission_denied(std::move(message)).with("path", std::string{path});
  }
  return core::Error::io(std::move(message)).with("path", std::string{path}).with("detail", ec.message());
}

[[nodiscard]] std::int64_t file_time_ns(std::filesystem::file_time_type value) noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count();
}

[[nodiscard]] core::Result<std::uint64_t> hash_file_contents(const std::filesystem::path& path,
                                                             std::uintmax_t max_bytes) {
  auto input = std::ifstream{path, std::ios::binary};
  if (!input) {
    return std::unexpected(
        core::Error::io("failed to open skill snapshot file for signature").with("path", path.string()));
  }

  auto hash = std::uint64_t{14695981039346656037ULL};
  auto buffer = std::array<char, 4096>{};
  auto remaining = max_bytes;
  while (input && remaining > 0) {
    const auto chunk = std::min<std::uintmax_t>(remaining, buffer.size());
    input.read(buffer.data(), static_cast<std::streamsize>(chunk));
    const auto count = input.gcount();
    for (auto i = std::streamsize{0}; i < count; ++i) {
      hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(i)]);
      hash *= 1099511628211ULL;
    }
    remaining -= static_cast<std::uintmax_t>(count);
  }
  if (!input.eof() && remaining > 0) {
    return std::unexpected(
        core::Error::io("failed to read skill snapshot file for signature").with("path", path.string()));
  }
  return hash;
}

[[nodiscard]] core::Result<std::vector<SkillFileSignature>> read_directory_signature(std::string_view directory,
                                                                                     const LoaderOptions& options) {
  if (directory.empty()) {
    return std::unexpected(core::Error::invalid_argument("skill snapshot directory must not be empty"));
  }

  const auto root = std::filesystem::path{std::string{directory}};
  std::error_code ec;
  if (!std::filesystem::exists(root, ec)) {
    if (ec) {
      return std::unexpected(signature_filesystem_error("failed to inspect skill snapshot directory", directory, ec));
    }
    return std::vector<SkillFileSignature>{};
  }
  if (!std::filesystem::is_directory(root, ec)) {
    if (ec) {
      return std::unexpected(
          signature_filesystem_error("failed to inspect skill snapshot directory type", directory, ec));
    }
    return std::unexpected(
        core::Error::invalid_argument("skill snapshot path is not a directory").with("path", std::string{directory}));
  }

  auto entries = std::vector<SkillFileSignature>{};
  auto it = std::filesystem::directory_iterator{root, std::filesystem::directory_options::skip_permission_denied, ec};
  if (ec) {
    return std::unexpected(signature_filesystem_error("failed to scan skill snapshot directory", directory, ec));
  }
  const auto end = std::filesystem::directory_iterator{};
  for (; it != end; it.increment(ec)) {
    if (ec) {
      return std::unexpected(signature_filesystem_error("failed to scan skill snapshot directory", directory, ec));
    }

    const auto name = it->path().filename().string();
    if (!name.ends_with(kMarkdownExtension)) {
      continue;
    }

    std::error_code status_ec;
    if (!it->is_regular_file(status_ec)) {
      if (status_ec) {
        return std::unexpected(
            signature_filesystem_error("failed to inspect skill snapshot file type", it->path().string(), status_ec));
      }
      continue;
    }
    const auto size = it->file_size(status_ec);
    if (status_ec) {
      return std::unexpected(
          signature_filesystem_error("failed to inspect skill snapshot file size", it->path().string(), status_ec));
    }
    const auto mtime = it->last_write_time(status_ec);
    if (status_ec) {
      return std::unexpected(
          signature_filesystem_error("failed to inspect skill snapshot file mtime", it->path().string(), status_ec));
    }
    const auto signature_read_limit = options.max_frontmatter_bytes + options.max_body_bytes + 32U;
    auto content_hash = hash_file_contents(it->path(), signature_read_limit);
    if (!content_hash) {
      return std::unexpected(std::move(content_hash).error());
    }
    entries.push_back(SkillFileSignature{
        .name = name,
        .size_bytes = size,
        .mtime_ns = file_time_ns(mtime),
        .content_hash = *content_hash,
    });
  }
  std::ranges::sort(entries, {}, &SkillFileSignature::name);
  return entries;
}

void invalidate_signature_paths(std::string_view directory,
                                std::span<const SkillFileSignature> current,
                                const std::optional<std::vector<SkillFileSignature>>& previous) {
  const auto root = std::filesystem::path{std::string{directory}};
  for (const auto& entry : current) {
    io::invalidate_read_text_file_ranged_cache((root / entry.name).string());
  }
  if (!previous.has_value()) {
    return;
  }
  for (const auto& entry : *previous) {
    io::invalidate_read_text_file_ranged_cache((root / entry.name).string());
  }
}

#if defined(__linux__)
class UniqueFd {
public:
  explicit UniqueFd(int fd = -1) noexcept : fd_{fd} {}

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  UniqueFd(UniqueFd&& other) noexcept : fd_{other.release()} {}

  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  ~UniqueFd() {
    reset();
  }

  [[nodiscard]] int get() const noexcept {
    return fd_;
  }

  [[nodiscard]] int release() noexcept {
    const auto fd = fd_;
    fd_ = -1;
    return fd;
  }

  void reset(int next = -1) noexcept {
    if (fd_ >= 0) {
      static_cast<void>(::close(fd_));
    }
    fd_ = next;
  }

private:
  int fd_{-1};
};

struct SkillWatchState {
  UniqueFd fd;
};

struct SkillWatchDrain {
  bool dirty{false};
  bool watcher_invalidated{false};
};

[[nodiscard]] core::Error errno_error(std::string message, std::string_view path, int err) {
  if (err == ENOENT) {
    return core::Error::not_found(std::move(message)).with("path", std::string{path});
  }
  if (err == EACCES || err == EPERM) {
    return core::Error::permission_denied(std::move(message)).with("path", std::string{path});
  }
  if (err == ENOTDIR) {
    return core::Error::invalid_argument(std::move(message)).with("path", std::string{path});
  }
  return core::Error::io(std::move(message))
      .with("path", std::string{path})
      .with("detail", std::error_code{err, std::generic_category()}.message());
}

[[nodiscard]] core::Error filesystem_error(std::string message, std::string_view path, const std::error_code& ec) {
  return core::Error::io(std::move(message)).with("path", std::string{path}).with("detail", ec.message());
}

[[nodiscard]] bool event_affects_skill_snapshot(const inotify_event& event) {
  if ((event.mask & (IN_Q_OVERFLOW | IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED)) != 0U) {
    return true;
  }
  if (event.len == 0 || event.name[0] == '\0') {
    return true;
  }
  return std::string_view{event.name}.ends_with(kMarkdownExtension);
}

[[nodiscard]] core::Result<std::filesystem::path> validate_watch_directory(std::string_view directory) {
  if (directory.empty()) {
    return std::unexpected(core::Error::invalid_argument("skill watcher directory must not be empty"));
  }

  const auto path = std::filesystem::path{std::string{directory}};
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    if (ec) {
      return std::unexpected(filesystem_error("failed to inspect skill watcher directory", directory, ec));
    }
    return std::unexpected(
        core::Error::not_found("skill watcher directory does not exist").with("path", std::string{directory}));
  }
  if (!std::filesystem::is_directory(path, ec)) {
    if (ec) {
      return std::unexpected(filesystem_error("failed to inspect skill watcher directory type", directory, ec));
    }
    return std::unexpected(
        core::Error::invalid_argument("skill watcher path is not a directory").with("path", std::string{directory}));
  }

  auto canonical = std::filesystem::weakly_canonical(path, ec);
  if (ec) {
    return std::unexpected(filesystem_error("failed to canonicalise skill watcher directory", directory, ec));
  }
  return canonical;
}
#endif

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

class WorkspaceSkillSnapshot::Impl {
public:
  Impl(asio::any_io_executor executor, std::string directory, LoaderOptions options)
      : executor_{std::move(executor)}, directory_{std::move(directory)}, options_{options},
        loader_{executor_, options} {}

  [[nodiscard]] async::Awaitable<core::Result<void>> refresh() {
    if (!executor_) {
      co_return std::unexpected(core::Error::invalid_argument("workspace skill snapshot requires an executor"));
    }
    if (directory_.empty()) {
      co_return std::unexpected(core::Error::invalid_argument("workspace skill snapshot directory must not be empty"));
    }

    auto current_signature = read_directory_signature(directory_, options_);
    if (!current_signature) {
      co_return std::unexpected(std::move(current_signature).error());
    }
    auto should_reload = !stats_.loaded;
    if (!signature_.has_value() || *current_signature != *signature_) {
      should_reload = true;
    }
#if defined(__linux__)
    auto watched = ensure_watcher();
    if (!watched) {
      if (watched.error().kind() == core::ErrorKind::not_found) {
        watcher_.reset();
        stats_.watcher_active = false;
      } else {
        watcher_.reset();
        stats_.watcher_active = false;
      }
    } else if (watcher_) {
      auto drained = drain_watcher();
      if (!drained) {
        co_return std::unexpected(std::move(drained).error());
      }
      should_reload = should_reload || drained->dirty;
    }
#endif

    if (!should_reload) {
      co_return core::Result<void>{};
    }

    invalidate_signature_paths(directory_, std::span<const SkillFileSignature>{*current_signature}, signature_);
    auto documents = co_await loader_.load_directory(directory_);
    if (!documents) {
      co_return std::unexpected(std::move(documents).error());
    }
    auto entries = catalog_entries_from(std::span<const SkillDocument>{*documents});
    auto catalog = render_catalog(entries);
    if (!catalog) {
      co_return std::unexpected(std::move(catalog).error());
    }

    documents_ = std::move(*documents);
    catalog_ = std::move(*catalog);
    signature_ = std::move(*current_signature);
    ++stats_.loads;
    stats_.loaded = true;
    co_return core::Result<void>{};
  }

  [[nodiscard]] const std::vector<SkillDocument>& documents() const noexcept {
    return documents_;
  }

  [[nodiscard]] const RenderedCatalog& catalog() const noexcept {
    return catalog_;
  }

  [[nodiscard]] WorkspaceSkillSnapshotStats stats() const noexcept {
    return stats_;
  }

private:
#if defined(__linux__)
  [[nodiscard]] core::Result<void> ensure_watcher() {
    if (watcher_) {
      return {};
    }

    auto directory = validate_watch_directory(directory_);
    if (!directory) {
      return std::unexpected(std::move(directory).error());
    }

    auto fd = UniqueFd{::inotify_init1(IN_NONBLOCK | IN_CLOEXEC)};
    if (fd.get() < 0) {
      return std::unexpected(errno_error("failed to create skill watcher", directory_, errno));
    }

    errno = 0;
    const auto wd = ::inotify_add_watch(fd.get(), directory->c_str(), kSkillWatchMask);
    if (wd < 0) {
      return std::unexpected(errno_error("failed to add skill watcher", directory->string(), errno));
    }

    watcher_ = SkillWatchState{.fd = std::move(fd)};
    stats_.watcher_active = true;
    return {};
  }

  [[nodiscard]] core::Result<SkillWatchDrain> drain_watcher() {
    auto result = SkillWatchDrain{};
    if (!watcher_) {
      return result;
    }

    alignas(inotify_event) std::array<char, kSkillWatchBufferBytes> buffer{};
    for (;;) {
      errno = 0;
      const auto bytes_read = ::read(watcher_->fd.get(), buffer.data(), buffer.size());
      if (bytes_read < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          if (result.watcher_invalidated) {
            watcher_.reset();
            stats_.watcher_active = false;
          }
          return result;
        }
        if (errno == EINTR) {
          continue;
        }
        return std::unexpected(errno_error("failed to read skill watcher events", directory_, errno));
      }
      if (bytes_read == 0) {
        if (result.watcher_invalidated) {
          watcher_.reset();
          stats_.watcher_active = false;
        }
        return result;
      }

      std::size_t offset = 0;
      const auto size = static_cast<std::size_t>(bytes_read);
      while (offset + sizeof(inotify_event) <= size) {
        const auto* event = reinterpret_cast<const inotify_event*>(buffer.data() + offset);
        offset += sizeof(inotify_event) + event->len;
        ++stats_.watcher_events;

        if (event_affects_skill_snapshot(*event)) {
          result.dirty = true;
          ++stats_.watcher_invalidations;
        }
        if ((event->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF | IN_Q_OVERFLOW)) != 0U) {
          result.watcher_invalidated = true;
        }
      }
    }
  }

  std::optional<SkillWatchState> watcher_{};
#endif

  asio::any_io_executor executor_{};
  std::string directory_;
  LoaderOptions options_{};
  Loader loader_;
  std::vector<SkillDocument> documents_;
  RenderedCatalog catalog_;
  std::optional<std::vector<SkillFileSignature>> signature_{};
  WorkspaceSkillSnapshotStats stats_{};
};

WorkspaceSkillSnapshot::WorkspaceSkillSnapshot(asio::any_io_executor executor,
                                               std::string directory,
                                               LoaderOptions options)
    : impl_{std::make_unique<Impl>(std::move(executor), std::move(directory), options)} {}

WorkspaceSkillSnapshot::~WorkspaceSkillSnapshot() = default;

WorkspaceSkillSnapshot::WorkspaceSkillSnapshot(WorkspaceSkillSnapshot&&) noexcept = default;

WorkspaceSkillSnapshot& WorkspaceSkillSnapshot::operator=(WorkspaceSkillSnapshot&&) noexcept = default;

async::Awaitable<core::Result<void>> WorkspaceSkillSnapshot::refresh() {
  return impl_->refresh();
}

const std::vector<SkillDocument>& WorkspaceSkillSnapshot::documents() const noexcept {
  return impl_->documents();
}

const RenderedCatalog& WorkspaceSkillSnapshot::catalog() const noexcept {
  return impl_->catalog();
}

WorkspaceSkillSnapshotStats WorkspaceSkillSnapshot::stats() const noexcept {
  return impl_->stats();
}

}  // namespace orangutan::skill
