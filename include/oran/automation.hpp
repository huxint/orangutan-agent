// include/oran/automation.hpp — public facade for oran-automation.
//
// The current automation surface ships deterministic periodic cadence, memory
// retention planning primitives, durable job/run/lease state, a caller-owned
// automation runtime handle, a caller-driven retention tick owner with optional
// retention hook metadata, and caller-started leased retention loop steps plus
// finite loop policy. Cron parsing, triggered jobs, queue/backpressure policy,
// and detached background service loops land behind this boundary in later
// slices.

#pragma once

#include <oran/automation/loop.hpp>
#include <oran/automation/periodic.hpp>
#include <oran/automation/repository.hpp>
#include <oran/automation/runtime.hpp>
#include <oran/automation/service.hpp>
