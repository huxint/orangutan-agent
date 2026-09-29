// Bounded live-test entry point. event.json is supplied by trusted local ingress.
#include <array>
#include <csignal>
#include <filesystem>
#include <print>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/signal_set.hpp>
#include <asio/this_coro.hpp>
#include <nlohmann/json.hpp>
#include <oran/async/runtime.hpp>
#include <oran/bootstrap/agent_session.hpp>
#include <oran/bootstrap/channel_http.hpp>
#include <oran/bootstrap/provider_backend.hpp>
#include <oran/bootstrap/qq.hpp>
#include <oran/bootstrap/runtime_assembly.hpp>
#include <oran/config/config.hpp>
#include <oran/hook/bus.hpp>
#include <oran/io/blocking.hpp>
#include <oran/io/private_directory.hpp>
#include <sodium/utils.h>

namespace {
using namespace orangutan;
using Json = nlohmann::json;
using core::Result;
using core::Error;
struct Wipe {
  std::string& value;
  ~Wipe() { sodium_memzero(value.data(), value.size()); }
};
permission::RuleSet rules() {
  permission::RuleSet result;
  for (const auto* operation : {"QQEvalRead", "QQEvalWrite", "QQToken", "ChannelReceive", "ChannelSend"})
    result.push_back({.verdict = permission::Verdict::allow, .tool_pattern = operation});
  return result;
}
async::Awaitable<Result<void>> authorize(hook::Bus& hooks, bool write) {
  const auto cancellation = co_await asio::this_coro::cancellation_state;
  if (cancellation.cancelled() != asio::cancellation_type::none)
    co_return std::unexpected(Error::cancelled());
  const auto operation = write ? "QQEvalWrite" : "QQEvalRead";
  const std::array capabilities{write ? core::Capability::write_file : core::Capability::read_file};
  auto decision = permission::evaluate(rules(), operation, capabilities, permission::Mode::strict);
  co_await hooks.publish_advisory(hook::Event::channel_action,
                                  hook::ChannelActionPayload{"qq", "live-eval", operation,
                                                            decision.verdict == permission::Verdict::allow});
  if (decision.verdict != permission::Verdict::allow)
    co_return std::unexpected(Error::permission_denied("QQ evaluation denied"));
  co_return Result<void>{};
}
async::Awaitable<Result<void>> save(io::PrivateDirectory& directory, const Json& journal,
                                    hook::Bus& hooks, asio::any_io_executor worker) {
  auto allowed = co_await authorize(hooks, true);
  if (!allowed)
    co_return std::unexpected(allowed.error());
  auto bytes = journal.dump(2);
  co_return co_await io::run_blocking(worker, [&directory, &bytes](std::stop_token) {
    return directory.write("live-journal.json", bytes);
  });
}
async::Awaitable<Result<void>> run(io::PrivateDirectory& directory, const std::string& workspace,
                                   bootstrap::RuntimeAssembly& assembly, config::Config& config,
                                   bootstrap::HttpProviderBackend& provider, http::Client& http,
                                   asio::any_io_executor worker) {
  auto& hooks = assembly.hook_bus();
  auto allowed = co_await authorize(hooks, false);
  if (!allowed)
    co_return std::unexpected(allowed.error());
  auto read = [&](std::string name, std::size_t limit) -> async::Awaitable<Result<std::optional<std::string>>> {
    co_return co_await io::run_blocking(worker, [&directory, &name, limit](std::stop_token) {
      return directory.read(name, limit);
    });
  };
  auto credentials = co_await read("qq-credentials.json", 16384);
  if (!credentials || !*credentials)
    co_return std::unexpected(Error::not_found("QQ binding missing"));
  Wipe bytes_wipe{**credentials};
  auto binding = Json::parse(**credentials);
  auto app = binding.at("app_id").get<std::string>();
  auto owner = binding.at("user_openid").get<std::string>();
  auto secret = binding.at("app_secret").get<std::string>();
  Wipe secret_wipe{secret};
  Wipe binding_wipe{binding.at("app_secret").get_ref<std::string&>()};
  if (owner.empty() || owner.size() > 256)
    co_return std::unexpected(Error::permission_denied("scanning user identity required"));
  auto input = co_await read("event.json", 1024 * 1024);
  if (!input || !*input)
    co_return std::unexpected(Error::not_found("private event.json missing"));
  auto decoded = channel::qq().decode(**input, {app, {}});
  if (!decoded || !*decoded)
    co_return std::unexpected(Error::invalid_argument("unsupported QQ event"));
  const auto& message = **decoded;
  if (message.conversation.kind != channel::ChatKind::direct || message.sender != owner ||
      message.conversation.chat != owner || !message.conversation.thread.empty() || message.text.empty() ||
      message.image)
    co_return std::unexpected(Error::permission_denied("only scanning user's private text messages are admitted"));
  auto previous = co_await read("live-journal.json", 4 * 1024 * 1024);
  if (!previous)
    co_return std::unexpected(previous.error());
  Json journal;
  core::TurnId identity{};
  if (*previous) {
    journal = Json::parse(**previous);
    if (journal.at("app") != app || journal.at("owner") != owner || journal.at("workspace") != workspace ||
        !journal.at("pending").is_null() || !journal.at("delivered").is_array())
      co_return std::unexpected(Error{core::ErrorKind::conflict, "binding changed or pending delivery unresolved"});
    auto parsed = core::parse_turn_id_hex(journal.at("session").get<std::string>());
    if (!parsed || core::is_zero_turn_id(*parsed))
      co_return std::unexpected(Error::parsing("invalid saved session"));
    identity = *parsed;
    for (const auto& delivered : journal.at("delivered")) {
      if (delivered.at("id") == message.event_id) {
        std::println("QQ duplicate already delivered");
        co_return Result<void>{};
      }
    }
    if (journal.at("delivered").size() >= 16)
      co_return std::unexpected(Error::invalid_argument("16-turn evaluation limit reached"));
  } else {
    auto generated = core::generate_turn_id();
    if (!generated)
      co_return std::unexpected(generated.error());
    identity = *generated;
    journal = {{"app", app}, {"owner", owner}, {"workspace", workspace},
               {"session", core::format_turn_id_hex(identity)}, {"pending", nullptr}, {"delivered", Json::array()}};
  }
  auto token = bootstrap::QQTokenSource::create({.app_id = app, .app_secret = std::move(secret),
      .executor = co_await asio::this_coro::executor,
      .send = [&http](http::BodyRequest request) { return http.send(std::move(request)); },
      .hooks = &hooks, .rules = rules()});
  if (!token)
    co_return std::unexpected(token.error());
  auto outbound = bootstrap::channel_http_transport(http, [&](channel::Conversation conversation) {
    return (*token)->get(std::move(conversation));
  });
  bootstrap::AgentSessionOptions options;
  options.executor = co_await asio::this_coro::executor;
  options.blocking_executor = worker;
  options.assembly = &assembly;
  options.config = &config;
  options.provider = &provider.system();
  options.route = provider.route();
  options.session_id = identity;
  options.scope_key = channel::conversation_key(message.conversation);
  options.agent_key = "qq-live-eval";
  options.identity = owner;
  options.origin = "qq";
  options.max_child_runs = 0;
  options.per_agent_overlay = "You are responding in a private QQ chat. Reply in the user's language.";
  auto session = bootstrap::AgentSession::create(std::move(options));
  if (!session)
    co_return std::unexpected(session.error());
  auto dispatcher = channel::Dispatcher::create(channel::qq(),
      [&](channel::Conversation conversation, channel::Request request) -> async::Awaitable<Result<channel::Response>> {
        const bool sending = Json::parse(request.body).value("msg_type", -1) == 0;
        if (sending) {
          journal["pending"]["send_inflight"] = true;
          auto written = co_await save(directory, journal, hooks, worker);
          if (!written)
            co_return std::unexpected(written.error());
        }
        auto response = co_await outbound(std::move(conversation), std::move(request));
        if (sending && response) {
          auto receipt = channel::qq().accept(*response);
          if (receipt) {
            journal["pending"]["receipts"].push_back(receipt->id);
            journal["pending"]["send_inflight"] = false;
            auto written = co_await save(directory, journal, hooks, worker);
            if (!written)
              co_return std::unexpected(written.error());
          }
        }
        co_return response;
      },
      [&](channel::Message incoming) -> async::Awaitable<Result<std::string>> {
        journal["pending"] = {{"id", incoming.event_id}, {"text", incoming.text}, {"answer", nullptr},
                              {"send_inflight", false}, {"receipts", Json::array()}};
        auto written = co_await save(directory, journal, hooks, worker);
        if (!written)
          co_return std::unexpected(written.error());
        auto answer = co_await (*session)->run_prompt({.prompt = incoming.text});
        if (!answer)
          co_return std::unexpected(answer.error());
        journal["pending"]["answer"] = answer->text;
        written = co_await save(directory, journal, hooks, worker);
        if (!written)
          co_return std::unexpected(written.error());
        co_return std::move(answer->text);
      }, {.rules = rules(), .hooks = &hooks});
  if (!dispatcher)
    co_return std::unexpected(dispatcher.error());
  auto result = co_await (*dispatcher)->handle(message);
  if (!result)
    co_return std::unexpected(result.error());
  journal["delivered"].push_back(journal["pending"]);
  journal["pending"] = nullptr;
  auto written = co_await save(directory, journal, hooks, worker);
  if (!written)
    co_return std::unexpected(written.error());
  std::println("QQ delivered: {} parts; persisted turns: {}", result->parts_sent, journal["delivered"].size());
  co_return Result<void>{};
}
}  // namespace

int main(int argc, char** argv) try {
  if (argc != 4) {
    std::println("eval-qq CONFIG WORKSPACE PRIVATE_STATE\n"
                 "Process private event.json from trusted authenticated ingress, using qq-credentials.json.\n"
                 "Only scanning user's private text; at most 16 turns; pending delivery blocks reuse.");
    return argc == 2 && std::string_view{argv[1]} == "--help" ? 0 : 2;
  }
  const auto workspace = std::filesystem::canonical(argv[2]).string();
  const auto state = std::filesystem::canonical(argv[3]);
  const auto relative = state.lexically_relative(workspace);
  if (relative.empty() || *relative.begin() != "..")
    return 2;
  auto directory = io::PrivateDirectory::open(state.string());
  if (!directory)
    return 2;
  auto lock = directory->lock("login.lock");
  if (!lock)
    return 2;
  auto config = config::Config::load_file(argv[1], {.strict_unknown_fields = true});
  if (!config || !config->permissions().workspace.extra_read_roots.empty() ||
      !config->permissions().workspace.extra_write_roots.empty())
    return 2;
  async::Runtime runtime{{.io_workers = 1, .cpu_workers = 4}};
  auto strand = runtime.make_strand();
  http::Client http{runtime.cpu_executor()};
  auto provider = bootstrap::HttpProviderBackend::build(*config,
      {.blocking_executor = runtime.cpu_executor(), .request_timeout = std::chrono::seconds{60}});
  if (!provider)
    return 2;
  auto assembly = bootstrap::RuntimeAssembly::build(workspace, runtime.cpu_executor(),
      {.audit_db_path = (state / "live-audit.db").string(),
       .sessions_db_path = (state / "live-sessions.db").string(),
       .longterm_memory_db_path = (state / "live-memory.db").string()});
  if (!assembly)
    return 2;
  asio::cancellation_signal cancellation;
  asio::signal_set signals{strand, SIGINT, SIGTERM};
  signals.async_wait([&](const asio::error_code& error, int) {
    if (!error)
      cancellation.emit(asio::cancellation_type::all);
  });
  int code = 1;
  asio::co_spawn(strand, run(*directory, workspace, *assembly, *config, *provider, http, runtime.cpu_executor()),
      asio::bind_cancellation_slot(cancellation.slot(), [&](std::exception_ptr exception, Result<void> result) {
        if (!exception && result)
          code = 0;
        else
          std::println(stderr, "QQ evaluation stopped; inspect private live-journal.json before retrying");
        signals.cancel();
        runtime.stop();
      }));
  return runtime.run() ? code : 1;
} catch (...) {
  std::println(stderr, "QQ evaluation setup failed");
  return 2;
}
