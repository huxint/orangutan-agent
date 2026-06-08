// include/oran/automation.hpp — public facade for oran-automation.
//
// The current automation surface ships deterministic periodic and cron cadence,
// memory retention planning primitives, durable retention job/run/lease state
// plus durable cron and triggered job/run/lease state, a caller-owned automation
// runtime handle, explicit cron seed application, a caller-awaited cron service
// cycle, a caller-owned composed automation service owner that drains buffered
// triggered work before one explicit cron cycle, caller-driven cron
// tick/execute owners, explicit cron wait/run loop steps, caller-driven
// triggered intake/execution plus bounded triggered queue/backpressure,
// non-blocking queue polling, finite batch draining with optional triggered
// agent leases, retention/cron/triggered hook metadata, caller-owned
// cron/triggered notifier callbacks, and caller-started leased retention loop
// steps plus finite loop policy. Concrete cli/channel/desktop notifier
// routing, agent firing, and detached background service loops land behind
// this boundary in later slices.

#pragma once

#include <oran/automation/loop.hpp>
#include <oran/automation/periodic.hpp>
#include <oran/automation/prompt.hpp>
#include <oran/automation/queue.hpp>
#include <oran/automation/repository.hpp>
#include <oran/automation/runtime.hpp>
#include <oran/automation/service.hpp>
