# Runtime Composition

`oran-bootstrap` exposes `RuntimeAssembly`, `HttpProviderBackend` and
`AgentSession` through `<oran/bootstrap.hpp>`. Applications compose these services
from explicit configuration, workspace, executors and session identity.

## Resource Ownership

The host owns the Asio runtime, provider transport, runtime assembly and session.
The session runs on a strand. Blocking filesystem, HTTP and SQLite operations
use the runtime's worker executor. The host awaits the
turn, including audit/trace writes and tool cleanup, before destroying borrowed
services.

`RuntimeAssembly` owns the workspace resolver, approval broker, hook bus, audit
sink and optional session/long-term repositories. Storage paths supplied by the
host select `audit.db`, `sessions.db` and `memory.db`; defaults are under
`<workspace>/.orangutan`. The host owns state-directory privacy and exclusion,
using `io::PrivateDirectory` and its lock when sharing persistent state. Existing
databases use versioned migrations.

`HttpProviderBackend` owns HTTP transport, one provider system and the resolved
route. It passes owned profile values and an explicit credential lookup to
`provider::make_protocol_system`. The [provider contract](api-portability.md)
owns construction validation and per-profile dispatch.

## Configuration Adapters

Bootstrap converts parsed configuration into owned runtime values before
execution. `materialize_permissions` accepts spans of global and optional agent
rules, prepends the mode's baseline and compiles input patterns. Workspace
settings are resolved separately. The returned RuleSet owns its strings and
patterns; materialization retains no rule views. Invalid patterns return an error without
exposing a partial policy. AgentSession passes the selected agent's rule view
directly without copying the surrounding permissions configuration.

`resolve_route_profiles` resolves configured route/profile names, protocol
aliases and model policy into `provider::RouteProfileResolution`. Explicit
protocols take precedence over vendor aliases. This conversion performs no
credential lookup or transport work. Provider construction then validates the
complete route before reading credentials.

These adapters are available through `<oran/bootstrap.hpp>` or their narrow
headers. Provider owns endpoint construction values and `SecretLookup` in
`protocol_transport.hpp`. Config retains parsed values only.

## Session Boundary

`AgentSession` loads bounded history, prepares an owned conversation through
`agent::prepare_conversation`, drives `agent::Loop`, and appends the successful
transcript suffix atomically through `Store::append_all`. The prepared value
records the history boundary independently of storage. The session borrows the
provider backend directly; the loop invokes provider execution for retries,
attribution and pricing. The session resolves
`memory.longterm.recall` from configuration and loads the scoped index through
MemoryRecall once before the loop by default. A supplied optional
`longterm_recall` value overrides that policy; exact caller framing replaces
default orientation. The returned index stays stable across model/tool
iterations, and the next prompt observes accepted memory updates. An unavailable
index is explicit context for the model; cancellation still ends the turn.

Memory adapters borrow one backend and capture the host's scope. They
do not capture session state or discover configuration. The session registers
filesystem tools and adds memory tools when memory services
exist. Recall and remember are visible in the default catalogue so the model can
inspect relevant durable notes by ID and save stable decisions or corrections
during ordinary work. The index, content reads and writes share the dispatch
boundary. [Memory](memory-system.md) owns projection, budgets, timestamps and
model-use guidance. Every turn joins its borrowed tool context before returning
or persisting.

Sessions share the broker, workspace, hook and audit services. Each session owns
its rule values. Configuration becomes an optional list of tool names at the
loop boundary; it does not enter prompt rendering or native protocol mapping.
An injected registry exposes custom tools through the same selection function.
The next prompt observes host registration changes. Without an approval consumer, an `ask`
decision fails closed. Embedders may bind an explicit approval sink through the
hook bus.

## Child Sessions

A delegating session registers `AgentRun` when configured agents exist and its
child budget is nonzero. The tool schema advertises those agent names. Its host
binding creates a fresh `AgentSession`, selects the child's permission and prompt
overlay, inherits the parent's mode and resource bindings, and attaches a borrowed
parent-policy view. The parent remains alive until all child calls finish.

Children share the parent's scheduler and strand, use a fresh approval identity,
and return their text plus agent/session identifiers. Child trace rows carry the
parent turn ID. Child token streams do not enter the parent's event sink; the
completed answer returns through the tool result. The child catalogue and any
explicit active-tool selection omit disabled delegation.

To permit delegation, authorize `AgentRun` through the configured permission
rules. The ordinary unmatched `ask` default still needs an approval consumer.
[Agent execution](agent-platform.md) owns admission and lifetime bounds.

## Hosting A Session

1. Parse configuration and create the executors. Use a strand for coordinating
   state and a worker executor for blocking operations.
2. Build `HttpProviderBackend` with the configuration and worker executor, then
   build `RuntimeAssembly` for the workspace and storage paths.
3. Create `AgentSession` with borrowed provider/assembly references, both
   executors, the provider route, scope and agent identities. Supply and retain
   a session ID and agent key for continuation; an all-zero ID starts a fresh
   session.
4. Await `run_prompt(PromptRequest)` on the coordinating strand and consume its
   `Result<PromptResult>`. The return boundary includes tool cleanup and
   successful transcript persistence.

`agent_config_name` selects configured permission and prompt overlays. The host
maps settings for workers, transport limits, hooks, trace and recall into the
corresponding construction options; [configuration](secrets-and-state.md) owns
that mapping. Stream output uses an optional `provider::EventSink`.

Host cancellation propagates through the awaiting coroutine. Tool dispatches and
storage writes finish before their borrowed services are released. The
[HTTP continuation test](../../tests/bootstrap/test_provider_backend.cpp) composes
these interfaces with a controlled transport and a reopened persistent session.
