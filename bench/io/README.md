# `bench/io/` — nanobench scenarios for `oran-io`

## What this bucket benchmarks

`oran-io` wraps blocking standard-library file work in the repository's coroutine
surface. The first scenario measures wrapper overhead against a direct blocking
read for a small text file.

## Scenarios

| File | A vs. B |
| --- | --- |
| [`scenarios/file_read.cpp`](scenarios/file_read.cpp) | Direct `std::ifstream` text read *vs.* `io::read_text_file` through an asio coroutine. |

## Running

```sh
xmake build bench-io
xmake run bench-io
```

Output is nanobench's markdown shape on stdout. Stable baseline JSON is still a
future benchmark-harness task.
