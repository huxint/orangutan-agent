#include "_impl/background-tasks.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/str.hpp>

namespace orangutan::bootstrap {
namespace {
using Json = nlohmann::json;
Json task_value(const TeamTaskSnapshot& task) {
  return {{"key", task.key},
          {"member", task.member},
          {"attempt", task.attempt},
          {"state", core::enum_name(task.state)},
          {"depends_on", task.depends_on},
          {"run_id", task.run_id},
          {"report_bytes", task.report.size()},
          {"report_truncated", task.report_truncated},
          {"error_kind", task.error_kind ? Json(core::enum_name(*task.error_kind)) : Json(nullptr)}};
}
}  // namespace

core::Result<tool::Output> team_output(const TeamSnapshot& snapshot, tool::TeamGetRequest request) {
  Json data{{"kind", "agent_team"},
            {"team_id", snapshot.team_id},
            {"stopped", snapshot.stopped},
            {"completion_delivery", snapshot.automatic_delivery ? "automatic" : "query_only"},
            {"survives_restart", false},
            {"sent_messages", snapshot.sent_messages},
            {"pending_messages", snapshot.pending_messages},
            {"tasks", Json::array()}};
  if (request.key.empty()) {
    data["members"] = Json::array();
    for (const auto& member : snapshot.members)
      data["members"].push_back({{"name", member.name}, {"tools", member.tools}});
    for (const auto& task : snapshot.tasks)
      data["tasks"].push_back(task_value(task));
  } else {
    const auto task = std::ranges::find(snapshot.tasks, request.key, &TeamTaskSnapshot::key);
    if (task == snapshot.tasks.end())
      return std::unexpected(core::Error::not_found("team task is unavailable"));
    if (request.offset > task->report.size() ||
        (request.offset < task->report.size() &&
         (static_cast<unsigned char>(task->report[request.offset]) & 0xc0U) == 0x80U))
      return std::unexpected(core::Error::invalid_argument("invalid team report byte offset"));
    auto report =
        core::str::truncate_to_code_point(std::string_view{task->report}.substr(request.offset), request.max_bytes);
    if (report.empty() && request.offset < task->report.size())
      return std::unexpected(core::Error::invalid_argument("team report window cannot fit one UTF-8 character"));
    auto value = task_value(*task);
    value["report"] = report;
    value["feedback"] = task->feedback;
    value["offset"] = request.offset;
    const auto next = request.offset + report.size();
    value["next_offset"] = next < task->report.size() ? Json(next) : Json(nullptr);
    data["tasks"].push_back(std::move(value));
  }
  auto text = data.dump();
  text += "\nA reported task awaits your review. Only accepting its current attempt unlocks dependents. "
          "Waiting can mean unaccepted dependencies, a busy member, or shared task capacity. "
          "Use TeamGet with a task key for evidence; capped reports are incomplete.";
  text += snapshot.automatic_delivery ? " Completion arrives automatically; do not poll just to wait."
                                      : " This host does not send automatic completion; inspect when needed.";
  return tool::Output{.text = std::move(text), .data_json = data.dump()};
}
}  // namespace orangutan::bootstrap
