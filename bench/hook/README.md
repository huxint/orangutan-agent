# bench-hook

`bench-hook` measures the cost of `hook::Bus::publish_advisory` and the
spec-0015 `publish_blocking<Event::tool_before>` path at varying fan-out.
The advisory cases are the baseline a future hook-bus optimisation would need
to beat; the blocking cases pin the cost of dispatch-time sink decisions.

A-vs-B-vs-C comparisons:

- `publish_no_sinks`: empty bus — publish is a map lookup + early return.
  The minimum cost a publish can ever have on the agent loop's critical path.
- `publish_one_sink`: bus + one `InProcessSink` subscribed to
  `tool_before`. Measures the per-sink dispatch overhead (await the sink's
  coroutine + record the SinkResult row).
- `publish_three_sinks`: same shape, three sinks subscribed — confirms that
  per-sink overhead is roughly linear in subscriber count and that the
  bus's `std::unordered_map` lookup is amortised.
- `publish_blocking_no_sinks`: empty blocking publish — the direct-dispatch
  no-policy floor for slice-91 `tool_before` consumption.
- `publish_blocking_one_sink`: one proceed sink — the single-sink blocking
  overhead relative to the advisory single-sink case.
- `publish_blocking_three_sinks_all_proceed`: three proceed sinks — linear
  all-clear fan-out for blocking hooks.
- `publish_blocking_short_circuit_second`: second sink vetoes — confirms the
  bus stops before later sinks and returns the decision trace collected so far.

The (publish_one_sink − publish_no_sinks) delta is the cost the agent loop
pays per sink it subscribes; the (publish_three_sinks − publish_one_sink) /
2 delta is the marginal cost of each additional sink. Both should be in the
sub-microsecond range — sinks themselves are cheap (a std::function call +
co_await on a coroutine that returns immediately); expensive sinks (shell,
webhook) pay their own cost on top.
