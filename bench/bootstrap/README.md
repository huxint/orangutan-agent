# Bootstrap Benchmark

Compares runtime assembly with storage-backed audit enabled and disabled.
Both cases exercise the same workspace and memory assembly, isolating audit
startup cost. Run `xmake build bench-bootstrap` then `xmake run bench-bootstrap`.
