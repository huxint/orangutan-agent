// src/oran-bootstrap/prompt_runner.cpp - bootstrap-owned CLI prompt runner.

#include <oran/bootstrap/prompt_runner.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <expected>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <oran/agent.hpp>
#include <oran/bootstrap/runtime_assembly.hpp>
#include <oran/cli/operator_prompt_sink.hpp>
#include <oran/cli/streaming_prompt_sink.hpp>
#include <oran/config.hpp>
#include <oran/core/content.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/core/message.hpp>
#include <oran/core/time.hpp>
#include <oran/hook.hpp>
#include <oran/memory.hpp>
#include <oran/permission.hpp>
#include <oran/provider.hpp>
#include <oran/skill.hpp>
#include <oran/storage.hpp>
#include <oran/tool.hpp>

namespace orangutan::bootstrap {
namespace {

using ::orangutan::core::Error;
using ::orangutan::core::Result;

[[nodiscard]] Error option_error(std::string message) {
  return Error::invalid_argument(std::move(message));
}

[[nodiscard]] Result<std::vector<memory::longterm::RecordKind>>
parse_longterm_recall_kinds(std::span<const std::string> names) {
  auto kinds = std::vector<memory::longterm::RecordKind>{};
  kinds.reserve(names.size());
  for (const auto& name : names) {
    auto parsed = core::parse_enum<memory::longterm::RecordKind>(name);
    if (!parsed) {
      return std::unexpected(option_error("agent prompt runner long-term recall kind is unknown").with("kind", name));
    }
    if (std::ranges::contains(kinds, *parsed)) {
      return std::unexpected(
          option_error("agent prompt runner long-term recall kind must be unique").with("kind", name));
    }
    kinds.push_back(*parsed);
  }
  return kinds;
}

[[nodiscard]] Result<std::vector<memory::longterm::RecordKind>>
parse_memory_tool_recall_kinds(std::span<const std::string> names) {
  auto kinds = std::vector<memory::longterm::RecordKind>{};
  kinds.reserve(names.size());
  for (const auto& name : names) {
    auto parsed = core::parse_enum<memory::longterm::RecordKind>(name);
    if (!parsed) {
      return std::unexpected(Error::invalid_argument("memory.recall: unknown kind").with("kind", name));
    }
    if (std::ranges::contains(kinds, *parsed)) {
      return std::unexpected(Error::invalid_argument("memory.recall: kind filters must be unique").with("kind", name));
    }
    kinds.push_back(*parsed);
  }
  return kinds;
}

[[nodiscard]] std::string user_message_text_for_recall(const core::Message& message) {
  if (message.role != core::Role::user) {
    return {};
  }

  std::string text;
  for (const auto& block : message.blocks) {
    const auto* content = std::get_if<core::TextContent>(&block);
    if (content == nullptr || content->text.empty()) {
      continue;
    }
    if (!text.empty()) {
      text.push_back('\n');
    }
    text.append(content->text);
  }
  return text;
}

[[nodiscard]] std::string last_user_message_text_for_recall(std::span<const core::Message> conversation_tail) {
  for (std::size_t i = conversation_tail.size(); i > 0; --i) {
    auto text = user_message_text_for_recall(conversation_tail[i - 1]);
    if (!text.empty()) {
      return text;
    }
  }
  return {};
}

[[nodiscard]] std::string recall_query_text(LongtermRecallQueryStrategy strategy,
                                            std::string_view prompt,
                                            std::span<const core::Message> conversation_tail) {
  switch (strategy) {
    case LongtermRecallQueryStrategy::prompt_text:
      return std::string{prompt};
    case LongtermRecallQueryStrategy::last_user_message: {
      auto text = last_user_message_text_for_recall(conversation_tail);
      if (!text.empty()) {
        return text;
      }
      return std::string{prompt};
    }
  }
  return std::string{prompt};
}

[[nodiscard]] Result<std::size_t> checked_cap(std::int64_t value, std::string field) {
  if (value < 0) {
    return std::unexpected(option_error("tool output cap must not be negative").with("field", std::move(field)));
  }
  if (static_cast<std::uint64_t>(value) > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return std::unexpected(option_error("tool output cap exceeds platform size range").with("field", std::move(field)));
  }
  return static_cast<std::size_t>(value);
}

[[nodiscard]] Result<core::TurnId> generate_session_id() {
  try {
    std::random_device random;
    core::TurnId id{};
    for (auto& byte : id) {
      byte = static_cast<std::byte>(static_cast<unsigned char>(random() & 0xffU));
    }
    if (core::is_zero_turn_id(id)) {
      id.back() = std::byte{0x01};
    }
    return id;
  } catch (const std::exception& error) {
    return std::unexpected(
        Error::internal("agent prompt runner: failed to generate session id").with("reason", error.what()));
  } catch (...) {
    return std::unexpected(
        Error::internal("agent prompt runner: failed to generate session id").with("reason", "unknown"));
  }
}

[[nodiscard]] std::string format_session_id(const core::TurnId& id) {
  constexpr std::string_view kHexDigits{"0123456789abcdef"};
  std::string out;
  out.reserve(id.size() * 2);
  for (auto byte : id) {
    const auto value = static_cast<unsigned char>(byte);
    out.push_back(kHexDigits[value >> 4]);
    out.push_back(kHexDigits[value & 0x0fu]);
  }
  return out;
}

[[nodiscard]] Result<permission::RuleSet>
materialize_runner_rules(const config::Config& cfg, permission::Mode mode, std::string_view permission_agent_name) {
  if (permission_agent_name.empty()) {
    return permission::materialize(mode, cfg.permissions());
  }

  const auto agents = cfg.agents();
  const auto match = std::ranges::find(agents, permission_agent_name, &config::AgentConfig::name);
  if (match == agents.end()) {
    return std::unexpected(Error::not_found("agent prompt runner permission overlay not found")
                               .with("agent", std::string{permission_agent_name}));
  }
  return permission::materialize(mode, cfg.permissions(), match->permissions);
}

[[nodiscard]] Result<const config::AgentConfig*> selected_agent_config(const config::Config& cfg,
                                                                       std::string_view agent_name) {
  if (agent_name.empty()) {
    return nullptr;
  }

  const auto agents = cfg.agents();
  const auto match = std::ranges::find(agents, agent_name, &config::AgentConfig::name);
  if (match == agents.end()) {
    return std::unexpected(
        Error::not_found("agent prompt runner agent config not found").with("agent", std::string{agent_name}));
  }
  return &*match;
}

[[nodiscard]] bool skill_enabled_for_agent(const skill::SkillDocument& document,
                                           const std::optional<std::vector<std::string>>& skills_enabled) {
  if (!skills_enabled.has_value()) {
    return true;
  }
  return std::ranges::contains(*skills_enabled, document.metadata.name);
}

[[nodiscard]] std::vector<skill::SkillDocument>
filter_skill_documents(std::span<const skill::SkillDocument> documents,
                       const std::optional<std::vector<std::string>>& skills_enabled) {
  auto filtered = std::vector<skill::SkillDocument>{};
  filtered.reserve(documents.size());
  for (const auto& document : documents) {
    if (skill_enabled_for_agent(document, skills_enabled)) {
      filtered.push_back(document);
    }
  }
  return filtered;
}

[[nodiscard]] Result<tool::OutputCapOptions> output_caps_from(const config::Config& cfg) {
  const auto max_text = checked_cap(cfg.runtime().tool_output.max_text_bytes, "runtime.tool_output.max_text_bytes");
  if (!max_text) {
    return std::unexpected(std::move(max_text).error());
  }
  const auto max_data = checked_cap(cfg.runtime().tool_output.max_data_bytes, "runtime.tool_output.max_data_bytes");
  if (!max_data) {
    return std::unexpected(std::move(max_data).error());
  }
  return tool::OutputCapOptions{
      .max_text_bytes = *max_text,
      .max_data_bytes = *max_data,
  };
}

[[nodiscard]] Result<agent::ToolSchedulerOptions> scheduler_options_from(const config::Config& cfg) {
  const auto& sched = cfg.runtime().tool_scheduler;
  auto max_parallel = checked_cap(sched.max_parallel_tools, "runtime.tool_scheduler.max_parallel_tools");
  if (!max_parallel) {
    return std::unexpected(std::move(max_parallel).error());
  }
  if (*max_parallel == 0) {
    return std::unexpected(option_error("runtime.tool_scheduler.max_parallel_tools must be positive"));
  }
  return agent::ToolSchedulerOptions{
      .max_parallel_tools = *max_parallel,
      .per_call_timeout = std::chrono::milliseconds{sched.per_call_timeout_ms},
      .idle_lock_ttl = std::chrono::milliseconds{sched.idle_lock_ttl_ms},
  };
}

[[nodiscard]] agent::SystemPreamble make_system_preamble(std::string text) {
  if (text.empty()) {
    return agent::default_system_preamble();
  }
  return agent::SystemPreamble{.section_text = std::move(text)};
}

[[nodiscard]] std::string render_skill_invocation_text(const skill::SkillDocument& document,
                                                       std::string_view inputs_json) {
  std::string text;
  text.reserve(document.metadata.name.size() + document.body.size() + inputs_json.size() + 64U);
  text.append("skill.invoke: ");
  text.append(document.metadata.name);
  text.append("\ninputs: ");
  text.append(inputs_json);
  text.append("\nbody:\n");
  text.append(document.body);
  return text;
}

[[nodiscard]] std::string render_skill_deactivation_text(std::string_view skill_name) {
  std::string text;
  text.reserve(skill_name.size() + 48U);
  text.append("skill.deactivate: ");
  text.append(skill_name);
  text.append("\nstatus: deactivated for this session");
  return text;
}

[[nodiscard]] std::string render_memory_recall_tool_text(const memory::longterm::RecallResult& recalled) {
  if (recalled.hits.empty()) {
    return "memory.recall: no matches";
  }

  std::string text;
  std::format_to(std::back_inserter(text),
                 "memory.recall: {} match{}\n",
                 recalled.hits.size(),
                 recalled.hits.size() == 1 ? "" : "es");
  text.append(recalled.framing.section_text);
  return text;
}

[[nodiscard]] std::vector<skill::SessionSkillActivation>
skill_policy_records_from(std::span<const memory::session::SkillActivationRecord> records) {
  auto out = std::vector<skill::SessionSkillActivation>{};
  out.reserve(records.size());
  for (const auto& record : records) {
    out.push_back(skill::SessionSkillActivation{.name = record.name, .active = record.active});
  }
  return out;
}

[[nodiscard]] std::vector<memory::session::SkillActivationUpdate>
skill_activation_updates_from_events(std::span<const skill::SkillActivationEvent> events) {
  auto updates = std::vector<memory::session::SkillActivationUpdate>{};
  updates.reserve(events.size());
  for (const auto& event : events) {
    updates.push_back(memory::session::SkillActivationUpdate{.name = event.name, .active = event.active});
  }
  return updates;
}

[[nodiscard]] Result<void> validate_options(const AgentPromptRunnerOptions& options) {
  if (options.assembly == nullptr) {
    return std::unexpected(option_error("agent prompt runner requires a runtime assembly"));
  }
  if (options.config == nullptr) {
    return std::unexpected(option_error("agent prompt runner requires a config"));
  }
  if (options.provider == nullptr) {
    return std::unexpected(option_error("agent prompt runner requires a provider system"));
  }
  if (!options.executor) {
    return std::unexpected(option_error("agent prompt runner requires an executor"));
  }
  if (options.route.primary.profile.empty()) {
    return std::unexpected(option_error("agent prompt runner route primary profile must not be empty"));
  }
  if (options.route.primary.model.empty()) {
    return std::unexpected(option_error("agent prompt runner route primary model must not be empty"));
  }
  if (options.scope_key.empty()) {
    return std::unexpected(option_error("agent prompt runner scope key must not be empty"));
  }
  if (options.agent_key.empty()) {
    return std::unexpected(option_error("agent prompt runner agent key must not be empty"));
  }
  if (options.identity.empty()) {
    return std::unexpected(option_error("agent prompt runner identity must not be empty"));
  }
  if (options.origin.empty()) {
    return std::unexpected(option_error("agent prompt runner origin must not be empty"));
  }
  if (options.trace_context_json.empty()) {
    return std::unexpected(option_error("agent prompt runner trace context JSON must not be empty"));
  }
  if (options.longterm_recall.enabled) {
    if (options.longterm_recall.limit == 0) {
      return std::unexpected(option_error("agent prompt runner long-term recall limit must be positive"));
    }
    if (!options.memory_framing.empty()) {
      return std::unexpected(
          option_error("agent prompt runner long-term recall cannot be combined with exact memory framing"));
    }
    if (options.assembly->longterm_memory_runtime() == nullptr) {
      return std::unexpected(option_error("agent prompt runner long-term recall requires long-term memory runtime"));
    }
  }
  return {};
}

}  // namespace

class AgentPromptRunner::Impl {
public:
  Impl(AgentPromptRunnerOptions options,
       tool::Registry registry,
       permission::RuleSet rules,
       config::PromptActiveToolsConfig active_tools,
       tool::OutputCapOptions output_caps,
       agent::ToolSchedulerOptions scheduler_options,
       std::optional<std::vector<std::string>> skills_enabled,
       std::vector<std::string> skills_deactivated,
       std::vector<skill::SkillExpiration> skills_expirations,
       std::vector<memory::longterm::RecordKind> longterm_recall_kinds,
       core::TurnId session_id)
      : executor_{std::move(options.executor)}, assembly_{options.assembly}, execution_runtime_{*options.provider},
        loop_{execution_runtime_, std::move(options.route)}, registry_{std::move(registry)},
        scheduler_{executor_, registry_, scheduler_options}, rules_{std::move(rules)},
        active_tools_{std::move(active_tools)}, output_caps_{output_caps}, session_id_{session_id},
        session_id_text_{format_session_id(session_id_)}, mode_{options.mode}, scope_key_{std::move(options.scope_key)},
        agent_key_{std::move(options.agent_key)}, identity_{std::move(options.identity)},
        origin_{std::move(options.origin)}, system_preamble_{make_system_preamble(std::move(options.system_preamble))},
        skills_catalog_{skill::RenderedCatalog{.section_text = std::move(options.skills_catalog)}},
        skills_enabled_{std::move(skills_enabled)}, skills_deactivated_{std::move(skills_deactivated)},
        skills_expirations_{std::move(skills_expirations)}, longterm_recall_{options.longterm_recall},
        longterm_recall_kinds_{std::move(longterm_recall_kinds)},
        memory_framing_{memory::Framing{.section_text = std::move(options.memory_framing)}},
        per_agent_overlay_{std::move(options.per_agent_overlay)},
        trace_context_json_{std::move(options.trace_context_json)}, tool_choice_{std::move(options.tool_choice)},
        max_tokens_{options.max_tokens}, thinking_budget_{options.thinking_budget}, retry_{options.retry},
        stream_{options.stream}, quiet_{options.quiet}, stream_out_{options.stream_out},
        operator_sink_{cli::OperatorPromptSinkOptions{
            .sink_id = "cli-operator-prompt",
            .operator_identity = identity_,
            .scripted_answers = std::move(options.approval_answers),
            .quiet = options.quiet,
        }} {
    if (!options.skills_directory.empty() && skills_catalog_.catalog().section_text.empty()) {
      skill_snapshot_.emplace(executor_, std::move(options.skills_directory));
    }
    assembly_->hook_bus().bind(operator_sink_, {hook::Event::permission_ask_rendered});
  }

  ~Impl() {
    if (assembly_ != nullptr) {
      assembly_->hook_bus().unbind(operator_sink_);
    }
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  [[nodiscard]] async::Awaitable<Result<cli::PromptRunResult>> run_prompt(cli::PromptRunRequest request) {
    auto catalog = registry_.catalog();
    auto conversation_tail = std::vector<core::Message>{};
    auto session_skill_activations = std::vector<memory::session::SkillActivationRecord>{};
    memory::session::Store* session_store = assembly_->session_store();
    if (session_store != nullptr) {
      auto loaded = co_await session_store->load(memory::session::SessionId{.value = session_id_text_},
                                                 memory::session::AgentKey{.value = agent_key_});
      if (!loaded) {
        co_return std::unexpected(std::move(loaded).error());
      }
      conversation_tail = std::move(*loaded);
      auto loaded_skill_activations =
          co_await session_store->load_skill_activations(memory::session::SessionId{.value = session_id_text_},
                                                         memory::session::AgentKey{.value = agent_key_});
      if (!loaded_skill_activations) {
        co_return std::unexpected(std::move(loaded_skill_activations).error());
      }
      session_skill_activations = std::move(*loaded_skill_activations);
    } else {
      conversation_tail = transcript_;
    }

    auto refreshed_catalog = co_await refresh_skill_catalog_snapshot(
        std::span<const core::Message>{conversation_tail},
        std::span<const memory::session::SkillActivationRecord>{session_skill_activations});
    if (!refreshed_catalog) {
      co_return std::unexpected(std::move(refreshed_catalog).error());
    }

    const auto prev_transcript_size = conversation_tail.size();
    auto prompt_text = std::move(request.prompt);
    const auto system_preamble = std::string{system_preamble_.render_once()};
    const auto skills_catalog = std::string{skills_catalog_.render_once()};
    auto memory_framing =
        co_await render_memory_framing_for_prompt(prompt_text, std::span<const core::Message>{conversation_tail});
    if (!memory_framing) {
      co_return std::unexpected(std::move(memory_framing).error());
    }
    conversation_tail.push_back(core::Message::user_text(std::move(prompt_text)));

    auto promotion_snapshot = session_state_.promotion_snapshot(core::time::now_utc());
    auto dispatch_context =
        tool::DispatchContext::for_now(executor_, rules_, assembly_->audit_sink(), scope_key_, agent_key_, identity_);
    dispatch_context.mode = mode_;
    dispatch_context.approval_broker = &assembly_->approval_broker();
    dispatch_context.bus = &assembly_->hook_bus();
    dispatch_context.skill_invoke = [this](std::string_view skill_name,
                                           std::string_view inputs_json,
                                           tool::DispatchContext& ctx) -> async::Awaitable<Result<tool::Output>> {
      co_return invoke_skill(skill_name, inputs_json, ctx);
    };
    dispatch_context.skill_deactivate = [this](std::string_view skill_name,
                                               tool::DispatchContext& ctx) -> async::Awaitable<Result<tool::Output>> {
      co_return deactivate_skill(skill_name, ctx);
    };
    dispatch_context.memory_recall = [this](tool::MemoryRecallRequest request,
                                            tool::DispatchContext& ctx) -> async::Awaitable<Result<tool::Output>> {
      co_return co_await recall_memory(std::move(request), ctx);
    };
    dispatch_context.workspace = &assembly_->workspace();
    dispatch_context.output_caps = output_caps_;

    auto inputs = agent::RunTurnInputs{
        .system_preamble = system_preamble,
        .tool_catalog = std::span<const core::ToolDef>{catalog},
        .active_tools = active_tools_,
        .promoted_tools = std::span<const std::string>{promotion_snapshot.tool_names},
        .skills_catalog = skills_catalog,
        .memory_framing = *memory_framing,
        .per_agent_overlay = per_agent_overlay_,
        .conversation_tail = std::span<const core::Message>{conversation_tail},
        .tool_choice = tool_choice_,
        .max_tokens = max_tokens_,
        .thinking_budget = thinking_budget_,
        .retry = retry_,
        .stream = stream_,
        .bus = &assembly_->hook_bus(),
        .scope_key = scope_key_,
        .agent_key = agent_key_,
        .identity = identity_,
        .origin = origin_,
        .tools = &registry_,
        .dispatch_context = &dispatch_context,
        .scheduler = &scheduler_,
    };

    if (auto* trace = assembly_->trace_repository(); trace != nullptr) {
      inputs.trace = agent::TraceContext{
          .repository = trace,
          .session_id = session_id_,
          .agent_key = agent_key_,
          .origin = origin_,
          .context_json = trace_context_json_,
      };
    }

    // Render streaming deltas live to the terminal when the operator can see
    // them. A quiet or non-streaming run passes no sink, so the loop still
    // assembles the same Response and the runner returns its text below.
    std::optional<cli::StreamingPromptSink> streaming_sink;
    provider::EventSink* sink = nullptr;
    if (stream_ && !quiet_) {
      streaming_sink.emplace(cli::StreamingPromptSinkOptions{.out = stream_out_});
      sink = &*streaming_sink;
    }

    auto result = co_await loop_.run_turn(std::move(inputs), sink);
    if (!result) {
      co_return std::unexpected(std::move(result).error());
    }

    observe_turn_results(result->transcript, prev_transcript_size);
    const auto skill_activation_events =
        skill::skill_activation_events_from_transcript(std::span<const core::Message>{result->transcript},
                                                       prev_transcript_size);
    auto skill_activation_updates =
        skill_activation_updates_from_events(std::span<const skill::SkillActivationEvent>{skill_activation_events});

    if (session_store != nullptr) {
      auto persisted = co_await append_transcript_suffix(*session_store, result->transcript, prev_transcript_size);
      if (!persisted) {
        co_return std::unexpected(std::move(persisted).error());
      }
      auto skill_activations = co_await persist_skill_activation_updates(
          *session_store,
          std::span<const memory::session::SkillActivationUpdate>{skill_activation_updates});
      if (!skill_activations) {
        co_return std::unexpected(std::move(skill_activations).error());
      }
    }

    transcript_ = std::move(result->transcript);
    ++prompts_processed_;

    auto text = std::move(result->text);
    if (streaming_sink && streaming_sink->rendered_answer_text()) {
      // The answer already appeared live; clear it so the CLI does not print
      // the same text a second time after the stream.
      text.clear();
    }
    co_return cli::PromptRunResult{.text = std::move(text)};
  }

  [[nodiscard]] std::size_t prompts_processed() const noexcept {
    return prompts_processed_;
  }

  [[nodiscard]] std::size_t approval_prompts_rendered() const noexcept {
    return operator_sink_.prompts_rendered();
  }

  [[nodiscard]] std::size_t tool_search_observations_recorded() const noexcept {
    return tool_search_observations_;
  }

  [[nodiscard]] std::size_t memory_framing_renders() const noexcept {
    return memory_framing_.stats().renders;
  }

  [[nodiscard]] std::size_t system_preamble_renders() const noexcept {
    return system_preamble_.stats().renders;
  }

  [[nodiscard]] std::size_t skill_catalog_renders() const noexcept {
    return skills_catalog_.stats().renders;
  }

  [[nodiscard]] std::size_t skill_catalog_loads() const noexcept {
    return skill_catalog_loads_;
  }

  [[nodiscard]] const provider::Route& route() const noexcept {
    return loop_.route();
  }

private:
  [[nodiscard]] async::Awaitable<Result<std::string>>
  render_memory_framing_for_prompt(std::string_view prompt, std::span<const core::Message> conversation_tail) {
    if (!longterm_recall_.enabled) {
      co_return std::string{memory_framing_.render_once()};
    }

    auto* runtime = assembly_->longterm_memory_runtime();
    if (runtime == nullptr) {
      co_return std::unexpected(option_error("agent prompt runner long-term recall runtime is unavailable"));
    }
    auto recalled = co_await runtime->recall(memory::longterm::RecallRequest{
        .query =
            memory::longterm::Query{
                .scope_key = scope_key_,
                .text = recall_query_text(longterm_recall_.query_strategy, prompt, conversation_tail),
                .kinds = longterm_recall_kinds_,
                .include_shadow = false,
            },
        .limit = longterm_recall_.limit,
    });
    if (!recalled) {
      co_return std::unexpected(std::move(recalled).error());
    }
    memory_framing_.replace(std::move(recalled->framing));
    co_return std::string{memory_framing_.render_once()};
  }

  [[nodiscard]] async::Awaitable<Result<void>>
  refresh_skill_catalog_snapshot(std::span<const core::Message> conversation_tail,
                                 std::span<const memory::session::SkillActivationRecord> session_skill_activations) {
    if (!skill_snapshot_.has_value()) {
      co_return Result<void>{};
    }
    auto refreshed = co_await skill_snapshot_->refresh();
    if (!refreshed) {
      co_return std::unexpected(std::move(refreshed).error());
    }
    skill_documents_ =
        filter_skill_documents(std::span<const skill::SkillDocument>{skill_snapshot_->documents()}, skills_enabled_);
    auto entries = skill::catalog_entries_from(std::span<const skill::SkillDocument>{skill_documents_});
    auto policy = skill::ActivationPolicy{};
    policy.session_skill_activations = skill_policy_records_from(session_skill_activations);
    policy.deactivated_skill_names = skills_deactivated_;
    policy.expirations = skills_expirations_;
    if (!policy.expirations.empty()) {
      policy.evaluation_time = core::time::now_utc();
    }
    auto active_skills = skill::resolve_active_skills(std::move(policy),
                                                      conversation_tail,
                                                      std::span<const skill::CatalogEntry>{entries});
    if (!active_skills) {
      co_return std::unexpected(std::move(active_skills).error());
    }
    auto catalog = skill::render_catalog(entries, *active_skills);
    if (!catalog) {
      co_return std::unexpected(std::move(catalog).error());
    }
    skills_catalog_.replace(std::move(*catalog));
    skill_catalog_loads_ = static_cast<std::size_t>(skill_snapshot_->stats().loads);
    co_return Result<void>{};
  }

  [[nodiscard]] Result<tool::Output>
  invoke_skill(std::string_view skill_name, std::string_view inputs_json, tool::DispatchContext& ctx) const {
    static_cast<void>(ctx);
    const auto match = std::ranges::find_if(skill_documents_, [skill_name](const skill::SkillDocument& document) {
      return document.metadata.name == skill_name;
    });
    if (match == skill_documents_.end()) {
      return std::unexpected(Error::not_found("skill.invoke: skill is not loaded")
                                 .with("skill", std::string{skill_name})
                                 .with("reason", "skill_not_loaded"));
    }
    auto data_json = skill::render_activation_data_json(match->metadata.name);
    if (!data_json) {
      return std::unexpected(std::move(data_json).error());
    }
    return tool::Output{
        .text = render_skill_invocation_text(*match, inputs_json),
        .data_json = std::move(*data_json),
        .attachments = {},
        .usage =
            tool::ToolUsage{
                .bytes_read = match->body.size(),
            },
        .is_error = false,
    };
  }

  [[nodiscard]] Result<tool::Output> deactivate_skill(std::string_view skill_name, tool::DispatchContext& ctx) const {
    static_cast<void>(ctx);
    const auto match = std::ranges::find_if(skill_documents_, [skill_name](const skill::SkillDocument& document) {
      return document.metadata.name == skill_name;
    });
    if (match == skill_documents_.end()) {
      return std::unexpected(Error::not_found("skill.deactivate: skill is not loaded")
                                 .with("skill", std::string{skill_name})
                                 .with("reason", "skill_not_loaded"));
    }
    auto data_json = skill::render_deactivation_data_json(match->metadata.name);
    if (!data_json) {
      return std::unexpected(std::move(data_json).error());
    }
    return tool::Output{
        .text = render_skill_deactivation_text(match->metadata.name),
        .data_json = std::move(*data_json),
        .attachments = {},
        .usage = {},
        .is_error = false,
    };
  }

  [[nodiscard]] async::Awaitable<Result<tool::Output>> recall_memory(tool::MemoryRecallRequest request,
                                                                     tool::DispatchContext& ctx) const {
    static_cast<void>(ctx);
    auto* runtime = assembly_->longterm_memory_runtime();
    if (runtime == nullptr) {
      co_return std::unexpected(Error::invalid_argument("memory.recall: runtime service is not available")
                                    .with("reason", "memory_runtime_unavailable"));
    }
    auto kinds = parse_memory_tool_recall_kinds(std::span<const std::string>{request.kinds});
    if (!kinds) {
      co_return std::unexpected(std::move(kinds).error());
    }
    auto recalled = co_await runtime->recall(memory::longterm::RecallRequest{
        .query =
            memory::longterm::Query{
                .scope_key = scope_key_,
                .text = std::move(request.query),
                .kinds = std::move(*kinds),
                .include_shadow = false,
            },
        .limit = request.limit,
    });
    if (!recalled) {
      co_return std::unexpected(std::move(recalled).error());
    }
    auto data_json =
        memory::longterm::render_recall_data_json(std::span<const memory::longterm::SearchHit>{recalled->hits});
    co_return tool::Output{
        .text = render_memory_recall_tool_text(*recalled),
        .data_json = std::move(data_json),
        .attachments = {},
        .usage =
            tool::ToolUsage{
                .match_count = static_cast<std::uint64_t>(recalled->hits.size()),
            },
        .is_error = false,
    };
  }

  [[nodiscard]] async::Awaitable<Result<void>> append_transcript_suffix(memory::session::Store& store,
                                                                        const std::vector<core::Message>& transcript,
                                                                        std::size_t start_index) const {
    for (std::size_t i = start_index; i < transcript.size(); ++i) {
      auto appended = co_await store.append(memory::session::SessionId{.value = session_id_text_},
                                            memory::session::AgentKey{.value = agent_key_},
                                            transcript[i]);
      if (!appended) {
        co_return std::unexpected(std::move(appended).error());
      }
    }
    co_return Result<void>{};
  }

  [[nodiscard]] async::Awaitable<Result<void>>
  persist_skill_activation_updates(memory::session::Store& store,
                                   std::span<const memory::session::SkillActivationUpdate> updates) const {
    for (const auto& update : updates) {
      auto recorded = co_await store.record_skill_activation(memory::session::SessionId{.value = session_id_text_},
                                                             memory::session::AgentKey{.value = agent_key_},
                                                             update);
      if (!recorded) {
        co_return std::unexpected(std::move(recorded).error());
      }
    }
    co_return Result<void>{};
  }

  // After a successful turn, walk the new transcript suffix for
  // (ToolUseContent, ToolResultContent) pairs and feed each result through
  // `agent::SessionState::observe_tool_output`. The session state filters by
  // tool name (only `tool.search` drives promotion), so the work is best-effort
  // and observation errors do not abort the prompt response — the loop has
  // already committed its audit/trace rows.
  void observe_turn_results(const std::vector<core::Message>& transcript, std::size_t start_index) {
    if (start_index > transcript.size()) {
      return;
    }
    std::unordered_map<std::string_view, std::string_view> tool_use_names;
    for (auto i = start_index; i < transcript.size(); ++i) {
      const auto& message = transcript[i];
      if (message.role != core::Role::assistant) {
        continue;
      }
      for (const auto& block : message.blocks) {
        if (const auto* use = std::get_if<core::ToolUseContent>(&block); use != nullptr) {
          tool_use_names.emplace(use->id, use->name);
        }
      }
    }

    const auto now = core::time::now_utc();
    for (auto i = start_index; i < transcript.size(); ++i) {
      const auto& message = transcript[i];
      if (message.role != core::Role::tool) {
        continue;
      }
      for (const auto& block : message.blocks) {
        const auto* result_block = std::get_if<core::ToolResultContent>(&block);
        if (result_block == nullptr) {
          continue;
        }
        const auto name_it = tool_use_names.find(result_block->tool_use_id);
        if (name_it == tool_use_names.end()) {
          continue;
        }
        const auto output = tool::Output{
            .text = result_block->output,
            .data_json = result_block->data_json,
            .attachments = {},
            .usage = {},
            .is_error = result_block->is_error,
        };
        auto report = session_state_.observe_tool_output(name_it->second, output, now);
        if (report.has_value() && report->observed_tool_search) {
          ++tool_search_observations_;
        }
      }
    }
  }

  asio::any_io_executor executor_;
  RuntimeAssembly* assembly_{};
  provider::execution::Runtime execution_runtime_;
  agent::Loop loop_;
  tool::Registry registry_;
  agent::ToolScheduler scheduler_;
  permission::RuleSet rules_;
  agent::SessionState session_state_;
  config::PromptActiveToolsConfig active_tools_;
  tool::OutputCapOptions output_caps_{};
  core::TurnId session_id_{};
  std::string session_id_text_;
  permission::Mode mode_{permission::Mode::default_};
  std::string scope_key_;
  std::string agent_key_;
  std::string identity_;
  std::string origin_;
  agent::SystemPreambleOwner system_preamble_;
  skill::CatalogOwner skills_catalog_;
  std::optional<std::vector<std::string>> skills_enabled_;
  std::vector<std::string> skills_deactivated_;
  std::vector<skill::SkillExpiration> skills_expirations_;
  std::optional<skill::WorkspaceSkillSnapshot> skill_snapshot_;
  std::vector<skill::SkillDocument> skill_documents_;
  LongtermRecallOptions longterm_recall_{};
  std::vector<memory::longterm::RecordKind> longterm_recall_kinds_;
  memory::FramingOwner memory_framing_;
  std::string per_agent_overlay_;
  std::string trace_context_json_;
  std::optional<std::string> tool_choice_{};
  std::optional<std::uint32_t> max_tokens_{};
  std::optional<std::uint32_t> thinking_budget_{};
  provider::RetryPolicy retry_{};
  bool stream_{true};
  bool quiet_{false};
  std::ostream* stream_out_{nullptr};
  cli::OperatorPromptSink operator_sink_;
  std::vector<core::Message> transcript_;
  std::size_t prompts_processed_{0};
  std::size_t tool_search_observations_{0};
  std::size_t skill_catalog_loads_{0};
};

core::Result<std::unique_ptr<AgentPromptRunner>> AgentPromptRunner::create(AgentPromptRunnerOptions options) {
  if (auto valid = validate_options(options); !valid) {
    return std::unexpected(std::move(valid).error());
  }
  auto longterm_recall_kinds = parse_longterm_recall_kinds(options.longterm_recall.kinds);
  if (!longterm_recall_kinds) {
    return std::unexpected(std::move(longterm_recall_kinds.error()));
  }

  auto registry = tool::Registry{};
  if (auto registered = tool::register_builtins(registry); !registered) {
    return std::unexpected(std::move(registered).error());
  }

  auto rules = materialize_runner_rules(*options.config, options.mode, options.permission_agent_name);
  if (!rules) {
    return std::unexpected(std::move(rules).error());
  }

  auto output_caps = output_caps_from(*options.config);
  if (!output_caps) {
    return std::unexpected(std::move(output_caps).error());
  }

  auto scheduler_options = scheduler_options_from(*options.config);
  if (!scheduler_options) {
    return std::unexpected(std::move(scheduler_options).error());
  }
  const auto skill_agent_name = options.agent_config_name.empty() ? std::string_view{options.permission_agent_name}
                                                                  : std::string_view{options.agent_config_name};
  auto agent_config = selected_agent_config(*options.config, skill_agent_name);
  if (!agent_config) {
    return std::unexpected(std::move(agent_config.error()));
  }
  auto skills_enabled = std::optional<std::vector<std::string>>{};
  auto skills_deactivated = std::vector<std::string>{};
  auto skills_expirations = std::vector<skill::SkillExpiration>{};
  if (*agent_config != nullptr) {
    skills_enabled = (*agent_config)->skills_enabled;
    if (options.per_agent_overlay.empty()) {
      options.per_agent_overlay = (*agent_config)->prompt_overlay;
    }
    skills_deactivated = (*agent_config)->skills_deactivated;
    skills_expirations.reserve((*agent_config)->skills_expirations.size());
    for (const auto& expiration : (*agent_config)->skills_expirations) {
      skills_expirations.push_back(
          skill::SkillExpiration{.name = expiration.name, .expires_at = expiration.expires_at});
    }
  }

  auto session_id = options.session_id;
  if (core::is_zero_turn_id(session_id)) {
    auto generated = generate_session_id();
    if (!generated) {
      return std::unexpected(std::move(generated).error());
    }
    session_id = *generated;
  }

  auto active_tools = options.config->runtime().prompt.active_tools;
  auto impl = std::make_unique<Impl>(std::move(options),
                                     std::move(registry),
                                     std::move(*rules),
                                     std::move(active_tools),
                                     *output_caps,
                                     *scheduler_options,
                                     std::move(skills_enabled),
                                     std::move(skills_deactivated),
                                     std::move(skills_expirations),
                                     std::move(*longterm_recall_kinds),
                                     session_id);
  return std::make_unique<AgentPromptRunner>(std::move(impl), AgentPromptRunner::PrivateTag{});
}

AgentPromptRunner::AgentPromptRunner(std::unique_ptr<Impl> impl, PrivateTag) noexcept : impl_{std::move(impl)} {}

AgentPromptRunner::~AgentPromptRunner() = default;

async::Awaitable<core::Result<cli::PromptRunResult>> AgentPromptRunner::run_prompt(cli::PromptRunRequest request) {
  return impl_->run_prompt(std::move(request));
}

std::size_t AgentPromptRunner::prompts_processed() const noexcept {
  return impl_->prompts_processed();
}

std::size_t AgentPromptRunner::approval_prompts_rendered() const noexcept {
  return impl_->approval_prompts_rendered();
}

std::size_t AgentPromptRunner::tool_search_observations_recorded() const noexcept {
  return impl_->tool_search_observations_recorded();
}

std::size_t AgentPromptRunner::memory_framing_renders() const noexcept {
  return impl_->memory_framing_renders();
}

std::size_t AgentPromptRunner::system_preamble_renders() const noexcept {
  return impl_->system_preamble_renders();
}

std::size_t AgentPromptRunner::skill_catalog_renders() const noexcept {
  return impl_->skill_catalog_renders();
}

std::size_t AgentPromptRunner::skill_catalog_loads() const noexcept {
  return impl_->skill_catalog_loads();
}

const provider::Route& AgentPromptRunner::route() const noexcept {
  return impl_->route();
}

}  // namespace orangutan::bootstrap
