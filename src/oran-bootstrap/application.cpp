#include <oran/bootstrap/application.hpp>

#include <algorithm>
#include <csignal>
#include <filesystem>
#include <print>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/signal_set.hpp>

#include <oran/async/runtime.hpp>
#include <oran/bootstrap/agent_session.hpp>
#include <oran/bootstrap/provider_backend.hpp>
#include <oran/bootstrap/runtime_assembly.hpp>
#include <oran/config/config.hpp>
#include <oran/core/str.hpp>
#include <oran/io/private_directory.hpp>

namespace orangutan::bootstrap {

core::Result<agent::PromptResult> run_application(ApplicationOptions options) {
  if (options.prompt.empty() || options.prompt.size() > 16384 || !core::str::is_valid_utf8(options.prompt)) {
    return std::unexpected(core::Error::invalid_argument("prompt must contain 1–16384 bytes of UTF-8 text"));
  }
  auto config = config::Config::load_file(options.config_path, {.strict_unknown_fields = true});
  if (!config)
    return std::unexpected(std::move(config).error());
  auto session_id = options.session_id.empty() ? core::generate_turn_id() : core::parse_turn_id_hex(options.session_id);
  if (!session_id)
    return std::unexpected(std::move(session_id).error());

  try {
    const auto workspace = std::filesystem::canonical(options.workspace);
    const auto state_path =
        options.state_directory.empty() ? workspace / ".orangutan" : std::filesystem::absolute(options.state_directory);
    auto state = io::PrivateDirectory::open(state_path.string());
    if (!state)
      return std::unexpected(std::move(state).error());
    auto lock = state->lock("runtime.lock");
    if (!lock)
      return std::unexpected(std::move(lock).error());
    async::Runtime runtime{{.io_workers = 1, .cpu_workers = static_cast<std::size_t>(config->runtime().workers)}};
    auto executor = runtime.make_strand();
    auto backend = HttpProviderBackend::build(
        *config,
        {
            .blocking_executor = runtime.cpu_executor(),
            .request_timeout = std::chrono::milliseconds{config->runtime().request_timeout_ms},
            .max_stream_bytes = static_cast<std::uint64_t>(config->runtime().stream.max_bytes),
        });
    if (!backend)
      return std::unexpected(std::move(backend).error());

    RuntimeAssemblyOptions assembly_options;
    assembly_options.audit_db_path = (state_path / "audit.db").string();
    assembly_options.sessions_db_path = (state_path / "sessions.db").string();
    assembly_options.longterm_memory_db_path = (state_path / "memory.db").string();
    assembly_options.workspace_options.extra_read_roots = config->permissions().workspace.extra_read_roots;
    assembly_options.workspace_options.extra_write_roots = config->permissions().workspace.extra_write_roots;
    assembly_options.trace_enabled = config->trace().enabled;
    assembly_options.hook_blocking_timeout = std::chrono::milliseconds{config->hooks().timeout_ms};
    auto assembly = RuntimeAssembly::build(workspace.string(), runtime.cpu_executor(), std::move(assembly_options));
    if (!assembly)
      return std::unexpected(std::move(assembly).error());

    AgentSessionOptions session_options;
    session_options.executor = executor;
    session_options.blocking_executor = runtime.cpu_executor();
    session_options.assembly = &*assembly;
    session_options.config = &*config;
    session_options.provider = &backend->system();
    session_options.route = backend->route();
    session_options.session_id = *session_id;
    session_options.scope_key = workspace.string();
    session_options.agent_key = options.agent_key;
    if (std::ranges::contains(config->agents(), options.agent_key, &config::AgentConfig::name)) {
      session_options.agent_config_name = options.agent_key;
    } else if (options.agent_key != "default") {
      return std::unexpected(core::Error::config("selected agent is not configured"));
    }
    session_options.longterm_recall.enabled = config->memory().longterm.recall.enabled;
    session_options.longterm_recall.limit = static_cast<std::size_t>(config->memory().longterm.recall.limit);
    session_options.longterm_recall.kinds = config->memory().longterm.recall.kinds;
    auto session = AgentSession::create(std::move(session_options));
    if (!session)
      return std::unexpected(std::move(session).error());

    core::Result<agent::PromptResult> result = std::unexpected(core::Error::internal("turn did not complete"));
    asio::cancellation_signal cancellation;
    asio::signal_set signals{executor, SIGINT, SIGTERM};
    signals.async_wait([&](const std::error_code& error, int) {
      if (!error)
        cancellation.emit(asio::cancellation_type::all);
    });
    asio::co_spawn(
        executor,
        (*session)->run_prompt({std::move(options.prompt)}),
        asio::bind_cancellation_slot(cancellation.slot(),
                                     [&](std::exception_ptr exception, core::Result<agent::PromptResult> completed) {
                                       result = exception ? core::Result<agent::PromptResult>{std::unexpected(
                                                                core::Error::internal("turn failed unexpectedly"))}
                                                          : std::move(completed);
                                       signals.cancel();
                                       runtime.stop();
                                     }));
    std::println(stderr, "session: {}", core::format_turn_id_hex(*session_id));
    auto ran = runtime.run();
    if (!ran)
      return std::unexpected(std::move(ran).error());
    return result;
  } catch (const std::exception&) {
    return std::unexpected(core::Error::internal("cannot run agent application"));
  }
}

}  // namespace orangutan::bootstrap
