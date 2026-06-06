// include/oran/automation.hpp — public facade for oran-automation.
//
// The first automation slice ships deterministic periodic cadence and memory
// retention planning primitives. The async service loop, cron parser,
// triggered jobs, leases, and automation.db persistence land behind this
// boundary in later slices.

#pragma once

#include <oran/automation/periodic.hpp>
