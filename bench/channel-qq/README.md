# `bench/channel-qq/` — nanobench scenarios for `oran-channel-qq`

## What this bucket benchmarks

`oran-channel-qq` wraps `oran-http::Client` with QQ-specific request building,
the platform retry ladder, and per-response normalization. The first scenario
prices that normalization layer; transport-throughput scenarios arrive with
the receive-transport milestone of
[`docs/exec-plans/active/2026-06-10-channel-qq-port.md`](../../docs/exec-plans/active/2026-06-10-channel-qq-port.md).

## Scenarios

| File | A vs. B |
| --- | --- |
| [`scenarios/normalize_response.cpp`](scenarios/normalize_response.cpp) | Empty-body normalization (header capture only) vs. JSON business-envelope normalization (header capture + nlohmann parse for `code`/`message`). |

## Running

```sh
xmake f --channel_qq=y
xmake build bench-channel-qq
xmake run bench-channel-qq
```

Output is nanobench's markdown shape on stdout. Stable baseline JSON is still a
future benchmark-harness task.
