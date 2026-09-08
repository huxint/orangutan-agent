#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>

#include <oran/async/channel.hpp>
#include <oran/hook.hpp>
#include <oran/permission.hpp>
#include <oran/tool.hpp>

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace hook = orangutan::hook;
namespace permission = orangutan::permission;
namespace tool = orangutan::tool;

namespace {

using namespace std::chrono_literals;

struct Call {
  Call(std::string name, std::string input, tool::DispatchContext context)
      : name{std::move(name)}, input{std::move(input)}, context{std::move(context)} {}

  std::string name;
  std::string input;
  tool::DispatchContext context;
  asio::cancellation_signal cancellation;
  std::optional<core::Result<tool::Output>> result;
  std::exception_ptr failure;
};

class Admission {
public:
  Admission() {
    rules.push_back(permission::Rule{.verdict = permission::Verdict::allow, .tool_pattern = "*"});
    std::filesystem::create_directory(root);
    auto created = tool::Workspace::create(root.string());
    REQUIRE(created.has_value());
    workspace = std::move(*created);
    REQUIRE(tool::register_file_read(registry).has_value());
    REQUIRE(tool::register_file_write(registry).has_value());
    sink.set_blocking_handler(
        [this](hook::Event, hook::PayloadPtr payload) -> async::Awaitable<core::Result<hook::HookDecision>> {
          const auto& before = std::get<hook::ToolBeforePayload>(*payload);
          if (auto it = decisions.find(before.who.identity); it != decisions.end()) {
            co_return it->second;
          }
          co_return hook::HookDecision{};
        });
    bus.bind(sink, {hook::Event::tool_before, hook::Event::tool_dispatched, hook::Event::tool_after});
  }

  ~Admission() {
    release.close();
    poll();
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }

  std::shared_ptr<Call>
  start(std::string name, std::string input, std::string identity, permission::AuditSink* audit_sink = nullptr) {
    auto context = tool::DispatchContext::for_now(io.get_executor(),
                                                  rules,
                                                  audit_sink ? *audit_sink : audit,
                                                  "scope",
                                                  "agent",
                                                  std::move(identity));
    context.workspace = &*workspace;
    context.path_locks = &locks;
    context.bus = &bus;
    auto call = std::make_shared<Call>(std::move(name), std::move(input), std::move(context));
    asio::co_spawn(io,
                   registry.dispatch(call->name, call->input, call->context),
                   asio::bind_cancellation_slot(call->cancellation.slot(),
                                                [call](std::exception_ptr failure, core::Result<tool::Output> result) {
                                                  call->failure = failure;
                                                  call->result = std::move(result);
                                                }));
    return call;
  }

  void rewrite(std::string identity, std::string input) {
    auto& decision = decisions[std::move(identity)];
    decision.kind = hook::HookDecisionKind::rewrite;
    decision.rewritten_input_json = std::move(input);
  }

  void poll() {
    io.restart();
    io.poll();
  }

  void finish_holder() {
    REQUIRE(release.try_send(true).has_value());
    poll();
  }

  std::string contents(std::string_view path) const {
    std::ifstream stream{root / path};
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
  }

  std::filesystem::path root{
      std::filesystem::temp_directory_path() /
      ("oran-admission-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
  asio::io_context io;
  async::Channel<bool> release{io.get_executor(), 1};
  tool::Registry registry;
  tool::PathLocks locks;
  std::optional<tool::Workspace> workspace;
  permission::RuleSet rules;
  permission::RecordingAuditSink audit;
  hook::Bus bus{{.advisory_timeout = 2s}};
  std::map<std::string, hook::HookDecision> decisions;
  std::vector<hook::ToolDispatchedPayload> dispatched;
  std::vector<hook::ToolAfterPayload> completed;
  hook::InProcessSink sink{"admission",
                           [this](hook::Event event, hook::PayloadPtr payload) -> async::Awaitable<core::Result<void>> {
                             if (event == hook::Event::tool_dispatched) {
                               const auto& call = std::get<hook::ToolDispatchedPayload>(*payload);
                               dispatched.push_back(call);
                               if (call.who.identity == "holder") {
                                 auto released = co_await release.receive();
                                 if (!released) {
                                   co_return std::unexpected(std::move(released).error());
                                 }
                               }
                             } else if (event == hook::Event::tool_after) {
                               completed.push_back(std::get<hook::ToolAfterPayload>(*payload));
                             }
                             co_return core::Result<void>{};
                           },
                           hook::SinkKind::trusted_local};
};

void require_success(const std::shared_ptr<Call>& call) {
  REQUIRE_FALSE(call->failure);
  REQUIRE(call->result.has_value());
  INFO((call->result->has_value() ? "success" : call->result->error().message()));
  REQUIRE(call->result->has_value());
}

class FailedAudit final : public permission::AuditSink {
public:
  async::Awaitable<core::Result<void>> record(permission::AuditEvent) override {
    co_return std::unexpected(core::Error::internal("audit unavailable"));
  }
};

}  // namespace

TEST_CASE("Tool admission excludes paths after input rewrite", "[integration][tool][admission][lock]") {
  Admission fixture;
  const std::string first_input = R"({"path":"shared.txt","content":"first"})";
  const std::string second_input = R"({"path":"./shared.txt","content":"second"})";
  fixture.rewrite("holder", first_input);
  fixture.rewrite("second", second_input);
  auto first = fixture.start("FileWrite", R"({"path":"a.txt","content":"unused"})", "holder");
  fixture.poll();
  auto second = fixture.start("FileWrite", R"({"path":"b.txt","content":"unused"})", "second");
  fixture.poll();
  const auto admitted_while_held = fixture.dispatched.size();
  const auto audited_while_held = fixture.audit.events().size();
  fixture.finish_holder();

  require_success(first);
  require_success(second);
  REQUIRE(admitted_while_held == 1);
  REQUIRE(audited_while_held == 1);
  REQUIRE(fixture.contents("shared.txt") == "second");
  REQUIRE_FALSE(std::filesystem::exists(fixture.root / "a.txt"));
  REQUIRE_FALSE(std::filesystem::exists(fixture.root / "b.txt"));
  REQUIRE(fixture.dispatched[0].input_json == first_input);
  REQUIRE(fixture.dispatched[1].input_json == second_input);
  REQUIRE(fixture.audit.events()[1].input_hash == permission::ApprovalAuthority::input_hash(second_input));
}

TEST_CASE("Tool admission permits overlap after rewriting to distinct paths", "[integration][tool][admission][lock]") {
  Admission fixture;
  fixture.rewrite("holder", R"({"path":"a.txt","content":"first"})");
  fixture.rewrite("second", R"({"path":"b.txt","content":"second"})");
  const std::string original = R"({"path":"shared.txt","content":"unused"})";
  auto first = fixture.start("FileWrite", original, "holder");
  fixture.poll();
  auto second = fixture.start("FileWrite", original, "second");
  fixture.poll();
  const bool overlapped = second->result.has_value() && !first->result.has_value();
  fixture.finish_holder();

  require_success(first);
  require_success(second);
  REQUIRE(overlapped);
  REQUIRE(fixture.contents("a.txt") == "first");
  REQUIRE(fixture.contents("b.txt") == "second");
  REQUIRE_FALSE(std::filesystem::exists(fixture.root / "shared.txt"));
}

TEST_CASE("Tool admission resolves a queued read after the writer creates its target",
          "[integration][tool][admission][workspace]") {
  Admission fixture;
  fixture.rewrite("reader", R"({"path":"created.txt"})");
  auto writer = fixture.start("FileWrite", R"({"path":"created.txt","content":"created under lock"})", "holder");
  fixture.poll();
  auto reader = fixture.start("FileRead", R"({"path":"original.txt"})", "reader");
  fixture.poll();
  const bool reader_waited = !reader->result.has_value();
  fixture.finish_holder();

  require_success(writer);
  require_success(reader);
  REQUIRE(reader_waited);
  REQUIRE(reader->result->value().text.contains("created under lock"));
  REQUIRE(fixture.completed.size() == 2);
}

TEST_CASE("Tool admission veto finishes without waiting for a held path", "[integration][tool][admission][hook]") {
  Admission fixture;
  fixture.decisions["veto"].kind = hook::HookDecisionKind::veto;
  fixture.decisions["veto"].reason = "blocked";
  auto holder = fixture.start("FileWrite", R"({"path":"shared.txt","content":"holder"})", "holder");
  fixture.poll();
  auto vetoed = fixture.start("FileWrite", R"({"path":"shared.txt","content":"veto"})", "veto");
  fixture.poll();
  const bool finished_before_release = vetoed->result.has_value();
  fixture.finish_holder();

  require_success(holder);
  REQUIRE(finished_before_release);
  REQUIRE_FALSE(vetoed->failure);
  REQUIRE_FALSE(vetoed->result->has_value());
  REQUIRE(vetoed->result->error().kind() == core::ErrorKind::permission_denied);
  REQUIRE(fixture.dispatched.size() == 1);
  REQUIRE(fixture.completed.size() == 2);
  REQUIRE(fixture.contents("shared.txt") == "holder");
}

TEST_CASE("Tool admission cancellation preserves approval and lets the next caller proceed",
          "[integration][tool][admission][cancellation][approval]") {
  Admission fixture;
  auto broker = permission::ApprovalBroker::with_random_secret();
  REQUIRE(broker.has_value());
  const std::string input = R"({"path":"shared.txt","content":"approved"})";
  auto token = broker->approve(permission::ApprovalGrant{.tool_name = "FileWrite",
                                                         .input = input,
                                                         .identity = "approved",
                                                         .ttl = 60s,
                                                         .replay_max = 1},
                               core::time::now_utc());
  auto holder = fixture.start("FileWrite", R"({"path":"shared.txt","content":"holder"})", "holder");
  fixture.poll();
  auto cancelled = fixture.start("FileWrite", input, "approved");
  cancelled->context.rules = {};
  cancelled->context.approval_broker = &*broker;
  cancelled->context.approval_token = &token;
  fixture.poll();
  const bool waited = !cancelled->result.has_value();
  cancelled->cancellation.emit(asio::cancellation_type::all);
  fixture.poll();
  const bool cancelled_while_held = cancelled->result.has_value();
  auto successor = fixture.start("FileWrite", input, "approved");
  successor->context.rules = {};
  successor->context.approval_broker = &*broker;
  successor->context.approval_token = &token;
  fixture.poll();
  fixture.finish_holder();

  require_success(holder);
  require_success(successor);
  REQUIRE(waited);
  REQUIRE(cancelled_while_held);
  REQUIRE_FALSE(cancelled->failure);
  REQUIRE_FALSE(cancelled->result->has_value());
  REQUIRE(cancelled->result->error().kind() == core::ErrorKind::cancelled);
  REQUIRE_FALSE(cancelled->context.resolved_path.has_value());
  REQUIRE(fixture.dispatched.size() == 2);
  REQUIRE(fixture.completed.size() == 3);
  REQUIRE(fixture.contents("shared.txt") == "approved");
  REQUIRE(fixture.audit.events().size() == 2);
}

TEST_CASE("Tool admission releases a refused or unaudited operation without effects",
          "[integration][tool][admission][permission][audit]") {
  Admission fixture;
  FailedAudit failed_audit;
  auto broker = permission::ApprovalBroker::with_random_secret();
  REQUIRE(broker.has_value());
  const std::string original = R"({"path":"original.txt","content":"unused"})";
  auto token = broker->approve(permission::ApprovalGrant{.tool_name = "FileWrite",
                                                         .input = original,
                                                         .identity = "refused",
                                                         .ttl = 60s,
                                                         .replay_max = 1},
                               core::time::now_utc());
  fixture.rewrite("refused", R"({"path":"final.txt","content":"refused"})");
  auto mode = permission::Mode::default_;
  bool unmatched = true;
  bool replay = false;
  permission::AuditSink* audit_sink = &fixture.audit;
  auto expected_kind = core::ErrorKind::permission_denied;
  SECTION("deny") {
    mode = permission::Mode::strict;
  }
  SECTION("approval required") {}
  SECTION("approval cannot authorize a different final input") {
    replay = true;
  }
  SECTION("audit failure") {
    audit_sink = &failed_audit;
    unmatched = false;
    expected_kind = core::ErrorKind::internal;
  }
  auto refused = fixture.start("FileWrite", original, "refused", audit_sink);
  refused->context.mode = mode;
  if (unmatched) {
    refused->context.rules = {};
  }
  if (replay) {
    refused->context.approval_broker = &*broker;
    refused->context.approval_token = &token;
  }
  fixture.poll();
  const bool no_effect = !std::filesystem::exists(fixture.root / "final.txt") && fixture.dispatched.empty();
  auto recovered = fixture.start("FileWrite", R"({"path":"final.txt","content":"recovered"})", "recovered");
  fixture.poll();

  REQUIRE(no_effect);
  REQUIRE_FALSE(refused->failure);
  REQUIRE(refused->result.has_value());
  REQUIRE_FALSE(refused->result->has_value());
  REQUIRE(refused->result->error().kind() == expected_kind);
  require_success(recovered);
  REQUIRE(fixture.contents("final.txt") == "recovered");
  REQUIRE_FALSE(std::filesystem::exists(fixture.root / "original.txt"));
}

TEST_CASE("Tool admission does not extend approval expiry while waiting for a path",
          "[integration][tool][admission][approval]") {
  Admission fixture;
  auto broker = permission::ApprovalBroker::with_random_secret();
  REQUIRE(broker.has_value());
  const std::string input = R"({"path":"shared.txt","content":"expired"})";
  auto token = broker->approve(permission::ApprovalGrant{.tool_name = "FileWrite",
                                                         .input = input,
                                                         .identity = "expiring",
                                                         .ttl = 1s,
                                                         .replay_max = 1},
                               core::Time::epoch());
  auto holder = fixture.start("FileWrite", R"({"path":"shared.txt","content":"holder"})", "holder");
  fixture.poll();
  auto expiring = fixture.start("FileWrite", input, "expiring");
  expiring->context.rules = {};
  expiring->context.approval_broker = &*broker;
  expiring->context.approval_token = &token;
  expiring->context.now = core::Time{token.expires_at.to_system_time_point() - 50ms};
  fixture.poll();
  fixture.io.restart();
  fixture.io.run_for(80ms);
  fixture.finish_holder();

  require_success(holder);
  REQUIRE_FALSE(expiring->failure);
  REQUIRE(expiring->result.has_value());
  REQUIRE_FALSE(expiring->result->has_value());
  REQUIRE(std::ranges::any_of(expiring->result->error().context(),
                              [](const auto& entry) { return entry.first == "reason" && entry.second == "expired"; }));
  REQUIRE(fixture.contents("shared.txt") == "holder");
  REQUIRE(fixture.dispatched.size() == 1);
}
