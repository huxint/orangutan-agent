// src/oran-agent/session_state.cpp — agent-owned prompt promotion state.

#include <oran/agent/session_state.hpp>

#include <cstddef>
#include <exception>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <oran/core/error.hpp>
#include <oran/tool/builtins.hpp>

namespace orangutan::agent {
namespace {

using json = nlohmann::json;

struct ToolSearchMatch {
  std::string name;
  bool deferred{false};
};

[[nodiscard]] core::Error malformed_tool_search_output(std::string reason) {
  return core::Error::invalid_argument("agent session: malformed tool.search output").with("reason", std::move(reason));
}

[[nodiscard]] core::Result<json> parse_data_json(const tool::Output& output) {
  if (!output.data_json.has_value()) {
    return std::unexpected(core::Error::invalid_argument("agent session: tool.search output is missing data_json"));
  }

  try {
    return json::parse(*output.data_json);
  } catch (const json::parse_error& e) {
    return std::unexpected(core::Error::invalid_argument("agent session: tool.search data_json is not valid JSON")
                               .with("detail", e.what()));
  } catch (const std::exception& e) {
    return std::unexpected(core::Error::invalid_argument("agent session: tool.search data_json is not valid JSON")
                               .with("detail", e.what()));
  }
}

[[nodiscard]] core::Result<std::vector<ToolSearchMatch>> parse_tool_search_matches(const tool::Output& output) {
  auto parsed = parse_data_json(output);
  if (!parsed) {
    return std::unexpected(std::move(parsed).error());
  }
  if (!parsed->is_object()) {
    return std::unexpected(malformed_tool_search_output("root is not an object"));
  }

  const auto kind_it = parsed->find("kind");
  if (kind_it == parsed->end() || !kind_it->is_string() || kind_it->get<std::string>() != "tool_search") {
    return std::unexpected(malformed_tool_search_output("kind is not tool_search"));
  }

  const auto matches_it = parsed->find("matches");
  if (matches_it == parsed->end() || !matches_it->is_array()) {
    return std::unexpected(malformed_tool_search_output("matches is not an array"));
  }

  std::vector<ToolSearchMatch> matches;
  matches.reserve(matches_it->size());
  for (std::size_t i = 0; i < matches_it->size(); ++i) {
    const auto& item = (*matches_it)[i];
    if (!item.is_object()) {
      return std::unexpected(malformed_tool_search_output("match is not an object").with("index", std::to_string(i)));
    }

    const auto name_it = item.find("name");
    if (name_it == item.end() || !name_it->is_string()) {
      return std::unexpected(
          malformed_tool_search_output("match name is not a string").with("index", std::to_string(i)));
    }
    auto name = name_it->get<std::string>();
    if (name.empty()) {
      return std::unexpected(malformed_tool_search_output("match name is empty").with("index", std::to_string(i)));
    }

    const auto deferred_it = item.find("deferred");
    if (deferred_it == item.end() || !deferred_it->is_boolean()) {
      return std::unexpected(
          malformed_tool_search_output("match deferred is not a boolean").with("index", std::to_string(i)));
    }

    matches.push_back(ToolSearchMatch{.name = std::move(name), .deferred = deferred_it->get<bool>()});
  }

  return matches;
}

}  // namespace

class SessionState::Impl {
public:
  explicit Impl(prompt::PromotionStateOptions promotion_options) : promotions_{std::move(promotion_options)} {}

  [[nodiscard]] core::Result<ToolPromotionReport>
  observe_tool_output(std::string_view tool_name, const tool::Output& output, core::Time now) {
    ToolPromotionReport report;
    if (tool_name != tool::kToolSearchName) {
      return report;
    }

    report.observed_tool_search = true;
    if (output.is_error) {
      return report;
    }

    auto matches = parse_tool_search_matches(output);
    if (!matches) {
      return std::unexpected(std::move(matches).error());
    }

    report.matches_seen = matches->size();
    for (const auto& match : *matches) {
      if (!match.deferred) {
        ++report.skipped_non_deferred;
        continue;
      }
      auto promoted = promotions_.promote(match.name, now);
      if (!promoted) {
        return std::unexpected(std::move(promoted).error());
      }
      ++report.promoted;
    }

    return report;
  }

  [[nodiscard]] prompt::PromotionSnapshot promotion_snapshot(core::Time now) {
    return promotions_.snapshot(now);
  }

  [[nodiscard]] prompt::PromotionStateStats promotion_stats() const noexcept {
    return promotions_.stats();
  }

  [[nodiscard]] const prompt::PromotionStateOptions& promotion_options() const noexcept {
    return promotions_.options();
  }

  void clear_promotions() {
    promotions_.clear();
  }

private:
  prompt::PromotionState promotions_;
};

SessionState::SessionState(prompt::PromotionStateOptions promotion_options)
    : impl_{std::make_unique<Impl>(std::move(promotion_options))} {}

SessionState::~SessionState() = default;

SessionState::SessionState(SessionState&&) noexcept = default;

SessionState& SessionState::operator=(SessionState&&) noexcept = default;

core::Result<ToolPromotionReport>
SessionState::observe_tool_output(std::string_view tool_name, const tool::Output& output, core::Time now) {
  return impl_->observe_tool_output(tool_name, output, now);
}

prompt::PromotionSnapshot SessionState::promotion_snapshot(core::Time now) {
  return impl_->promotion_snapshot(now);
}

prompt::PromotionStateStats SessionState::promotion_stats() const noexcept {
  return impl_->promotion_stats();
}

const prompt::PromotionStateOptions& SessionState::promotion_options() const noexcept {
  return impl_->promotion_options();
}

void SessionState::clear_promotions() {
  impl_->clear_promotions();
}

}  // namespace orangutan::agent
