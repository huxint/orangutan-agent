# `bench/automation`

Automation benchmarks currently cover the first deterministic periodic-planning
surface. The full service tick benchmark from spec 0006 lands when the
automation service owns persisted jobs and a scheduler loop.

| Scenario | Compares |
| --- | --- |
| [`scenarios/periodic.cpp`](scenarios/periodic.cpp) | `evaluate_periodic_schedule` over a 1024-job batch vs. `plan_memory_retention` over the same batch, pinning the overhead added by memory-retention request construction. |
