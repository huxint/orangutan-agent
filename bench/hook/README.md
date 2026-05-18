# bench-hook

`bench-hook` measures the cost of `hook::Bus::publish_advisory` at varying
fan-out, the baseline a future hook-bus optimisation would need to beat.

A-vs-B-vs-C comparisons:

- `publish_no_sinks`: empty bus — publish is a map lookup + early return.
  The minimum cost a publish can ever have on the agent loop's critical path.
- `publish_one_sink`: bus + one `InProcessSink` subscribed to
  `tool_before`. Measures the per-sink dispatch overhead (await the sink's
  coroutine + record the SinkResult row).
- `publish_three_sinks`: same shape, three sinks subscribed — confirms that
  per-sink overhead is roughly linear in subscriber count and that the
  bus's `std::unordered_map` lookup is amortised.

The (publish_one_sink − publish_no_sinks) delta is the cost the agent loop
pays per sink it subscribes; the (publish_three_sinks − publish_one_sink) /
2 delta is the marginal cost of each additional sink. Both should be in the
sub-microsecond range — sinks themselves are cheap (a std::function call +
co_await on a coroutine that returns immediately); expensive sinks (shell,
webhook) pay their own cost on top.
