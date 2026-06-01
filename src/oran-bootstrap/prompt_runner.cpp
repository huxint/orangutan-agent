// src/oran-bootstrap/prompt_runner.cpp - bootstrap-owned CLI prompt runner.

#include <oran/bootstrap/prompt_runner.hpp>

#include <chrono>
#include <cstddef>
#include <exception>
#include <expected>
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
       core::TurnId session_id)
      : executor_{std::move(options.executor)}, assembly_{options.assembly}, execution_runtime_{*options.provider},
        loop_{execution_runtime_, std::move(options.route)}, registry_{std::move(registry)},
        scheduler_{executor_, registry_, scheduler_options}, rules_{std::move(rules)},
        active_tools_{std::move(active_tools)}, output_caps_{output_caps}, session_id_{session_id},
        session_id_text_{format_session_id(session_id_)}, mode_{options.mode}, scope_key_{std::move(options.scope_key)},
        agent_key_{std::move(options.agent_key)}, identity_{std::move(options.identity)},
        origin_{std::move(options.origin)}, system_preamble_{make_system_preamble(std::move(options.system_preamble))},
        skills_catalog_{skill::RenderedCatalog{.section_text = std::move(options.skills_catalog)}},
        skills_directory_{std::move(options.skills_directory)},
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
    skills_catalog_loaded_ = skills_directory_.empty() || !skills_catalog_.catalog().section_text.empty();
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
    auto loaded_catalog = co_await ensure_skill_catalog_loaded();
    if (!loaded_catalog) {
      co_return std::unexpected(std::move(loaded_catalog).error());
    }

    auto catalog = registry_.catalog();
    auto conversation_tail = std::vector<core::Message>{};
    memory::session::Store* session_store = assembly_->session_store();
    if (session_store != nullptr) {
      auto loaded = co_await session_store->load(memory::session::SessionId{.value = session_id_text_},
                                                 memory::session::AgentKey{.value = agent_key_});
      if (!loaded) {
        co_return std::unexpected(std::move(loaded).error());
      }
      conversation_tail = std::move(*loaded);
    } else {
      conversation_tail = transcript_;
    }

    const auto prev_transcript_size = conversation_tail.size();
    conversation_tail.push_back(core::Message::user_text(std::move(request.prompt)));
    const auto system_preamble = std::string{system_preamble_.render_once()};
    const auto skills_catalog = std::string{skills_catalog_.render_once()};
    const auto memory_framing = std::string{memory_framing_.render_once()};

    auto promotion_snapshot = session_state_.promotion_snapshot(core::time::now_utc());
    auto dispatch_context = tool::DispatchContext{
        .executor = executor_,
        .mode = mode_,
        .rules = rules_,
        .audit = assembly_->audit_sink(),
        .approval_broker = &assembly_->approval_broker(),
        .now = core::time::now_utc(),
        .bus = &assembly_->hook_bus(),
        .skill_invoke = [this](std::string_view skill_name, std::string_view inputs_json, tool::DispatchContext& ctx)
            -> async::Awaitable<Result<tool::Output>> { co_return invoke_skill(skill_name, inputs_json, ctx); },
        .workspace = &assembly_->workspace(),
        .output_caps = output_caps_,
        .scope_key = scope_key_,
        .agent_key = agent_key_,
        .identity = identity_,
    };

    auto inputs = agent::RunTurnInputs{
        .system_preamble = system_preamble,
        .tool_catalog = std::span<const core::ToolDef>{catalog},
        .active_tools = active_tools_,
        .promoted_tools = std::span<const std::string>{promotion_snapshot.tool_names},
        .skills_catalog = skills_catalog,
        .memory_framing = memory_framing,
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

    if (session_store != nullptr) {
      auto persisted = co_await append_transcript_suffix(*session_store, result->transcript, prev_transcript_size);
      if (!persisted) {
        co_return std::unexpected(std::move(persisted).error());
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
  [[nodiscard]] async::Awaitable<Result<void>> ensure_skill_catalog_loaded() {
    if (skills_catalog_loaded_) {
      co_return Result<void>{};
    }
    if (skills_directory_.empty()) {
      skills_catalog_loaded_ = true;
      co_return Result<void>{};
    }

    auto loader = skill::Loader{executor_};
    auto documents = co_await loader.load_directory(skills_directory_);
    if (!documents) {
      co_return std::unexpected(std::move(documents).error());
    }
    auto entries = skill::catalog_entries_from(std::span<const skill::SkillDocument>{*documents});
    auto catalog = skill::render_catalog(entries);
    if (!catalog) {
      co_return std::unexpected(std::move(catalog).error());
    }
    skill_documents_ = std::move(*documents);
    skills_catalog_.replace(std::move(*catalog));
    skills_catalog_loaded_ = true;
    ++skill_catalog_loads_;
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
    return tool::Output{
        .text = render_skill_invocation_text(*match, inputs_json),
        .data_json = std::nullopt,
        .attachments = {},
        .usage =
            tool::ToolUsage{
                .bytes_read = match->body.size(),
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
  std::vector<skill::SkillDocument> skill_documents_;
  std::string skills_directory_;
  bool skills_catalog_loaded_{true};
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
