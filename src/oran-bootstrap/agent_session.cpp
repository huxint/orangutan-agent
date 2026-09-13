#include <oran/bootstrap/agent_session.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <span>
#include <system_error>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/error.hpp>
#include <asio/use_awaitable.hpp>

#include <oran/agent.hpp>
#include <oran/bootstrap/permissions.hpp>
#include <oran/bootstrap/runtime_assembly.hpp>
#include <oran/config.hpp>
#include <oran/memory/session.hpp>
#include <oran/provider.hpp>
#include <oran/tool.hpp>

#include "_impl/child-agents.hpp"
#include "memory_tools.hpp"

namespace orangutan::bootstrap {
namespace {

using core::Error;
using core::Result;

[[nodiscard]] Result<std::size_t> checked_cap(std::int64_t value, std::string field) {
  if (value < 0) {
    return std::unexpected(
        Error::invalid_argument("tool output cap must not be negative").with("field", std::move(field)));
  }
  if (static_cast<std::uint64_t>(value) > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return std::unexpected(
        Error::invalid_argument("tool output cap exceeds platform size range").with("field", std::move(field)));
  }
  return static_cast<std::size_t>(value);
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
        Error::not_found("agent session agent config not found").with("agent", std::string{agent_name}));
  }
  return &*match;
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

[[nodiscard]] Result<void> validate_options(const AgentSessionOptions& options) {
  if (options.assembly == nullptr) {
    return std::unexpected(Error::invalid_argument("agent session requires a runtime assembly"));
  }
  if (options.config == nullptr) {
    return std::unexpected(Error::invalid_argument("agent session requires a config"));
  }
  if (options.provider == nullptr) {
    return std::unexpected(Error::invalid_argument("agent session requires a provider system"));
  }
  if (!options.executor || !options.blocking_executor) {
    return std::unexpected(Error::invalid_argument("agent session requires an executor"));
  }
  if ((options.registry == nullptr) != (options.scheduler == nullptr)) {
    return std::unexpected(
        Error::invalid_argument("agent session registry and scheduler must be supplied together (both or neither)"));
  }
  if (options.route.primary.profile.empty()) {
    return std::unexpected(Error::invalid_argument("agent session route primary profile must not be empty"));
  }
  if (options.route.primary.model.empty()) {
    return std::unexpected(Error::invalid_argument("agent session route primary model must not be empty"));
  }
  if (options.scope_key.empty()) {
    return std::unexpected(Error::invalid_argument("agent session scope key must not be empty"));
  }
  if (options.agent_key.empty()) {
    return std::unexpected(Error::invalid_argument("agent session agent key must not be empty"));
  }
  if (options.identity.empty()) {
    return std::unexpected(Error::invalid_argument("agent session identity must not be empty"));
  }
  if (options.origin.empty()) {
    return std::unexpected(Error::invalid_argument("agent session origin must not be empty"));
  }
  if (options.trace_context_json.empty()) {
    return std::unexpected(Error::invalid_argument("agent session trace context JSON must not be empty"));
  }
  if (options.longterm_recall && options.longterm_recall->enabled) {
    if (options.longterm_recall->limit == 0 || options.longterm_recall->limit > 20) {
      return std::unexpected(Error::invalid_argument("agent session long-term recall limit must be between 1 and 20"));
    }
    if (!options.memory_framing.empty()) {
      return std::unexpected(
          Error::invalid_argument("agent session long-term recall cannot be combined with exact memory framing"));
    }
    if (options.assembly->longterm_memory_backend() == nullptr) {
      return std::unexpected(
          Error::invalid_argument("agent session long-term recall requires long-term memory backend"));
    }
  }
  return {};
}

}  // namespace

core::Result<agent::ToolSchedulerOptions> scheduler_options_from(const config::Config& config) {
  const auto& sched = config.runtime().tool_scheduler;
  auto max_parallel = checked_cap(sched.max_parallel_tools, "runtime.tool_scheduler.max_parallel_tools");
  if (!max_parallel) {
    return std::unexpected(std::move(max_parallel).error());
  }
  if (*max_parallel == 0) {
    return std::unexpected(Error::invalid_argument("runtime.tool_scheduler.max_parallel_tools must be positive"));
  }
  return agent::ToolSchedulerOptions{
      .max_parallel_tools = *max_parallel,
      .per_call_timeout = std::chrono::milliseconds{sched.per_call_timeout_ms},
  };
}

class AgentSession::Impl {
public:
  Impl(AgentSessionOptions options,
       std::optional<tool::Registry> owned_registry,
       permission::RuleSet rules,
       tool::OutputCapOptions output_caps,
       agent::ToolSchedulerOptions scheduler_options)
      : options_{std::move(options)}, loop_{*options_.provider, options_.route},
        owned_registry_{std::move(owned_registry)}, rules_{std::move(rules)}, output_caps_{output_caps},
        active_tools_{options_.config->runtime().prompt.active_tools},
        session_id_text_{core::format_turn_id_hex(options_.session_id)} {
    if (options_.scheduler != nullptr) {
      registry_ = options_.registry;
      scheduler_ = options_.scheduler;
    } else {
      registry_ = &*owned_registry_;
      owned_scheduler_.emplace(options_.executor, *registry_, scheduler_options);
      scheduler_ = &*owned_scheduler_;
    }
    if (options_.parent_policy || options_.max_child_runs == 0) {
      std::erase(active_tools_.tool_names, tool::AGENT_RUN_NAME);
    }
  }

  [[nodiscard]] async::Awaitable<Result<agent::PromptResult>> run_prompt(agent::PromptRequest request) {
    auto history = std::vector<core::Message>{};
    auto* store = options_.assembly->session_store();
    if (store != nullptr) {
      auto loaded = co_await asio::co_spawn(options_.blocking_executor,
                                            store->load_tail(memory::session::SessionId{.value = session_id_text_},
                                                             memory::session::AgentKey{.value = options_.agent_key}),
                                            asio::use_awaitable);
      if (!loaded) {
        co_return std::unexpected(std::move(loaded).error());
      }
      history = std::move(*loaded);
    } else {
      history = transcript_;
    }

    auto context = tool::DispatchContext::for_now(options_.blocking_executor,
                                                  rules_,
                                                  options_.assembly->audit_sink(),
                                                  options_.scope_key,
                                                  options_.agent_key,
                                                  options_.identity);
    context.mode = options_.mode;
    context.parent_policy = options_.parent_policy;
    context.approval_broker = &options_.assembly->approval_broker();
    context.bus = &options_.assembly->hook_bus();
    context.workspace = &options_.assembly->workspace();
    context.output_caps = output_caps_;
    std::size_t child_runs = 0;
    bind_child_agents(context, options_, *registry_, *scheduler_, child_runs);
    if (auto* backend = options_.assembly->longterm_memory_backend(); backend != nullptr) {
      bind_memory_tools(context, *backend, options_.scope_key);
    }

    auto memory_framing = options_.memory_framing;
    if (options_.longterm_recall->enabled) {
      auto recalled =
          co_await recall_prompt_memory(*registry_,
                                        context,
                                        tool::MemoryRecallRequest{.limit = options_.longterm_recall->limit,
                                                                  .kinds = options_.longterm_recall->kinds});
      if (!recalled) {
        co_return std::unexpected(std::move(recalled).error());
      }
      memory_framing = std::move(*recalled);
    }
    auto conversation = agent::prepare_conversation(std::move(history), std::move(request.prompt));
    auto catalog = registry_->catalog();
    if (!context.agent_run) {
      std::erase_if(catalog, [](const auto& definition) { return definition.name == tool::AGENT_RUN_NAME; });
    }
    std::optional<std::span<const std::string>> active_tools;
    if (!active_tools_.use_defaults) {
      active_tools = active_tools_.tool_names;
    }
    auto inputs = agent::RunTurnInputs{
        .system_preamble = options_.system_preamble,
        .tool_catalog = catalog,
        .active_tools = active_tools,
        .memory_framing = memory_framing,
        .per_agent_overlay = options_.per_agent_overlay,
        .conversation_tail = conversation.messages,
        .tool_choice = options_.tool_choice,
        .max_tokens = options_.max_tokens,
        .thinking_budget = options_.thinking_budget,
        .retry = options_.retry,
        .stream = options_.stream,
        .turn_id = request.turn_id,
        .bus = context.bus,
        .scope_key = options_.scope_key,
        .agent_key = options_.agent_key,
        .identity = options_.identity,
        .origin = options_.origin,
        .tools = registry_,
        .dispatch_context = &context,
        .scheduler = scheduler_,
    };
    if (auto* trace = options_.assembly->trace_repository(); trace != nullptr) {
      inputs.trace = agent::TraceContext{
          .repository = trace,
          .blocking_executor = options_.blocking_executor,
          .session_id = options_.session_id,
          .parent_turn_id = options_.parent_turn_id,
          .agent_key = options_.agent_key,
          .origin = options_.origin,
          .context_json = options_.trace_context_json,
      };
    }

    Result<agent::RunTurnResult> result = std::unexpected(Error::internal("agent turn did not finish"));
    try {
      result = co_await loop_.run_turn(std::move(inputs), options_.event_sink);
    } catch (const std::system_error& error) {
      result = std::unexpected(error.code() == asio::error::operation_aborted
                                   ? Error::cancelled()
                                   : Error::internal("agent turn failed unexpectedly"));
    } catch (const std::exception&) {
      result = std::unexpected(Error::internal("agent turn failed unexpectedly"));
    }
    // The turn frame owns the context borrowed by every scheduled dispatch.
    auto drained = co_await scheduler_->wait_idle(&context);
    if (!drained) {
      co_return std::unexpected(std::move(drained).error());
    }
    if (!result) {
      co_return std::unexpected(std::move(result).error());
    }
    if (store != nullptr) {
      auto persisted =
          co_await asio::co_spawn(options_.blocking_executor,
                                  store->append_all(memory::session::SessionId{.value = session_id_text_},
                                                    memory::session::AgentKey{.value = options_.agent_key},
                                                    std::span{result->transcript}.subspan(conversation.history_size)),
                                  asio::use_awaitable);
      if (!persisted) {
        co_return std::unexpected(std::move(persisted).error());
      }
    }
    if (store == nullptr) {
      transcript_ = std::move(result->transcript);
    }
    co_return agent::PromptResult{.text = std::move(result->text)};
  }

  [[nodiscard]] const provider::Route& route() const noexcept {
    return loop_.route();
  }

private:
  AgentSessionOptions options_;
  agent::Loop loop_;
  std::optional<tool::Registry> owned_registry_;
  std::optional<agent::ToolScheduler> owned_scheduler_;
  tool::Registry* registry_{};
  agent::ToolScheduler* scheduler_{};
  permission::RuleSet rules_;
  tool::OutputCapOptions output_caps_;
  config::PromptActiveToolsConfig active_tools_;
  std::string session_id_text_;
  std::vector<core::Message> transcript_;
};

core::Result<std::unique_ptr<AgentSession>> AgentSession::create(AgentSessionOptions options) {
  if (!options.longterm_recall && options.config != nullptr && options.assembly != nullptr) {
    const auto& recall = options.config->memory().longterm.recall;
    options.longterm_recall = LongtermRecallOptions{
        .enabled =
            recall.enabled && options.assembly->longterm_memory_backend() != nullptr && options.memory_framing.empty(),
        .limit = static_cast<std::size_t>(recall.limit),
        .kinds = recall.kinds,
    };
  }
  if (auto valid = validate_options(options); !valid) {
    return std::unexpected(std::move(valid).error());
  }
  auto selected = selected_agent_config(*options.config, options.agent_config_name);
  if (!selected) {
    return std::unexpected(std::move(selected).error());
  }
  const auto agent_rules =
      *selected ? std::span{(*selected)->permissions.rules} : std::span<const config::PermissionRuleConfig>{};
  auto rules = materialize_permissions(options.mode, options.config->permissions().rules, agent_rules);
  if (!rules) {
    return std::unexpected(std::move(rules).error());
  }
  if (*selected && options.per_agent_overlay.empty()) {
    options.per_agent_overlay = (*selected)->prompt_overlay;
  }
  auto output_caps = output_caps_from(*options.config);
  if (!output_caps) {
    return std::unexpected(std::move(output_caps).error());
  }
  auto scheduler_options = scheduler_options_from(*options.config);
  if (!scheduler_options) {
    return std::unexpected(std::move(scheduler_options).error());
  }
  auto registry = std::optional<tool::Registry>{};
  if (options.scheduler == nullptr) {
    registry.emplace();
    if (auto added = tool::register_builtins(*registry); !added) {
      return std::unexpected(std::move(added).error());
    }
    if (options.assembly->longterm_memory_backend() != nullptr) {
      if (auto added = tool::register_memory_tools(*registry); !added) {
        return std::unexpected(std::move(added).error());
      }
    }
    if (!options.parent_policy && options.max_child_runs > 0 && !options.config->agents().empty()) {
      auto agent_names = std::vector<std::string>{};
      for (const auto& agent : options.config->agents()) {
        agent_names.push_back(agent.name);
      }
      if (auto added = tool::register_agent_run(*registry, agent_names); !added) {
        return std::unexpected(std::move(added).error());
      }
    }
  }
  if (core::is_zero_turn_id(options.session_id)) {
    auto generated = core::generate_turn_id();
    if (!generated) {
      return std::unexpected(std::move(generated).error());
    }
    options.session_id = *generated;
  }
  auto impl = std::make_unique<Impl>(std::move(options),
                                     std::move(registry),
                                     std::move(*rules),
                                     *output_caps,
                                     *scheduler_options);
  return std::make_unique<AgentSession>(std::move(impl), PrivateTag{});
}

AgentSession::AgentSession(std::unique_ptr<Impl> impl, PrivateTag) noexcept : impl_{std::move(impl)} {}
AgentSession::~AgentSession() = default;

async::Awaitable<core::Result<agent::PromptResult>> AgentSession::run_prompt(agent::PromptRequest request) {
  try {
    co_return co_await impl_->run_prompt(std::move(request));
  } catch (const std::system_error& error) {
    co_return std::unexpected(error.code() == asio::error::operation_aborted
                                  ? Error::cancelled()
                                  : Error::internal("agent session failed unexpectedly"));
  } catch (const std::exception&) {
    co_return std::unexpected(Error::internal("agent session failed unexpectedly"));
  }
}

const provider::Route& AgentSession::route() const noexcept {
  return impl_->route();
}

}  // namespace orangutan::bootstrap
