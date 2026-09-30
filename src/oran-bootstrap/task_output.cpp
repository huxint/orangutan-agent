#include <oran/bootstrap/background_tasks.hpp>

#include <format>
#include <nlohmann/json.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/tool/output.hpp>

namespace orangutan::bootstrap {
tool::Output task_output(const TaskSnapshot& snapshot) {
  const auto delivery = snapshot.automatic_delivery ? "automatic" : "query_only";
  auto data = nlohmann::json{
      {"kind", "background_task"},
      {"task_id", snapshot.task_id},
      {"agent_key", snapshot.agent_key},
      {"label", snapshot.label},
      {"state", core::enum_name(snapshot.state)},
      {"completion_delivery", delivery},
      {"survives_turn", true},
      {"survives_restart", false},
      {"acknowledged", snapshot.acknowledged},
      {"result", snapshot.result},
      {"result_offset", snapshot.result_offset},
      {"result_bytes", snapshot.result_bytes},
      {"next_offset", snapshot.next_offset ? nlohmann::json(*snapshot.next_offset) : nlohmann::json(nullptr)},
      {"result_truncated", snapshot.result_truncated},
      {"error_kind",
       snapshot.error_kind ? nlohmann::json(core::enum_name(*snapshot.error_kind)) : nlohmann::json(nullptr)}};
  auto text = std::format("Task {} ({}) is {}. Completion delivery: {}.\n",
                          snapshot.task_id,
                          snapshot.label,
                          core::enum_name(snapshot.state),
                          delivery);
  if (snapshot.state == TaskState::queued || snapshot.state == TaskState::running) {
    text += "Accepted, not completed. Continues across turns, but not across process restarts.\n";
    text += snapshot.automatic_delivery
                ? "Continue independent work; the host will deliver completion. Do not poll just to wait.\n"
                : "Inspect later with TaskGet; this host does not arrange an automatic follow-up.\n";
  }
  if (snapshot.error_kind)
    text += std::format("Task ended with {}.\n", core::enum_name(*snapshot.error_kind));
  text += snapshot.result;
  if (snapshot.next_offset)
    text += std::format("\nMore result text: TaskGet offset={}.", *snapshot.next_offset);
  if (snapshot.result_truncated)
    text += "\n[Retained result was truncated.]";
  return {.text = std::move(text), .data_json = data.dump()};
}
}  // namespace orangutan::bootstrap
