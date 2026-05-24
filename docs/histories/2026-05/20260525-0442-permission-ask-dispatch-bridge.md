# Permission Ask Dispatch Bridge

The remaining spec-0015 approval gap was too broad to land as one slice:
`oran-tool` needed the generic dispatch bridge first, while the terminal prompt
UI belongs in `oran-cli`. This slice ships the dispatch-side bridge. When a
permission rule returns `ask`, a broker is available, no replay token was
supplied, and a bus is attached, `Registry::dispatch` now publishes blocking
`permission_ask_rendered` with a typed payload containing the tool call, identity,
decision reason, replay count, and TTL. A subscribed sink returning `proceed`
causes dispatch to issue and immediately verify an approval token, optionally
copy it into `DispatchContext::approval_token_output`, record
`outcome=approved`, and run the handler. A sink returning `veto` records
`outcome=rejected`, returns `reason=operator_denied`, and skips the handler.
With no subscribed ask sink, the legacy `approval_required` short-circuit remains
unchanged.

Design intent: keep approval rendering behind the same blocking-hook bus as
`tool_before` rather than adding a prompt-specific special case to the agent
loop. The concrete terminal UI sink is still downstream; this slice gives it a
typed payload and a broker/token handoff to consume.

Files of interest:

- `include/oran/hook/payload.hpp` — adds `PermissionAskRenderedPayload` to the
  public hook payload variant.
- `include/oran/tool/registry.hpp` — documents `approval_token_output` and the
  new ask-resolution branch.
- `src/oran-tool/registry.cpp` — publishes `permission_ask_rendered`, issues and
  verifies approval grants on `proceed`, rejects operator vetoes, and preserves
  the no-sink short-circuit.
- `src/oran-tool/audit_metadata.cpp` — serializes
  `metadata_json.permission_ask_decisions[]`.
- `tests/tool/test_registry.cpp` — covers no-sink preservation, proceed +
  replay-token output, and veto rejection.

Validation:

- `xmake run test-tool` — 178 cases / 1838 assertions.
