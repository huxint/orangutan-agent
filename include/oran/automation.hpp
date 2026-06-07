// include/oran/automation.hpp — public facade for oran-automation.
//
// The current automation surface ships deterministic periodic cadence, memory
// retention planning primitives, durable job/run state, and a caller-driven
// retention tick owner. Cron parsing, triggered jobs, leases, background service
// loops, and hook publication land behind this boundary in later slices.

#pragma once

#include <oran/automation/periodic.hpp>
#include <oran/automation/repository.hpp>
#include <oran/automation/service.hpp>
