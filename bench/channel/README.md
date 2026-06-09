# `bench/channel/` — nanobench scenarios for `oran-channel`

## What this bucket benchmarks

`oran-channel` owns the adapter trait and fan-in manager used by future QQ,
webhook, and team-chat adapters. The first scenario measures the manager's
bounded inbound handoff overhead against a direct in-memory append baseline.

## Scenarios

| File | A vs. B |
| --- | --- |
| [`scenarios/manager_fanin.cpp`](scenarios/manager_fanin.cpp) | Direct vector append vs. `ChannelManager::receive_one(...)` fan-in through `async::Channel<InboundMessage>`. |

## Running

```sh
xmake build bench-channel
xmake run bench-channel
```

Output is nanobench's markdown shape on stdout. Stable baseline JSON is still a
future benchmark-harness task.
