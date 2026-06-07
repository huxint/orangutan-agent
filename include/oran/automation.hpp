// include/oran/automation.hpp — public facade for oran-automation.
//
// The current automation surface ships deterministic periodic and cron cadence,
// memory retention planning primitives, durable retention job/run/lease state
// plus durable cron job state, a caller-owned automation runtime handle,
// explicit cron seed application, a caller-awaited cron service cycle,
// caller-driven cron tick/execute owners, explicit cron wait/run loop steps,
// retention and cron hook metadata, and caller-started leased retention loop
// steps plus finite loop policy. Triggered jobs, queue/backpressure policy, and
// detached background service loops land behind this boundary in later slices.

#pragma once

#include <oran/automation/loop.hpp>
#include <oran/automation/periodic.hpp>
#include <oran/automation/repository.hpp>
#include <oran/automation/runtime.hpp>
#include <oran/automation/service.hpp>
