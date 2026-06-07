// include/oran/automation.hpp — public facade for oran-automation.
//
// The first automation slice ships deterministic periodic cadence and memory
// retention planning primitives. The async service loop, cron parser,
// triggered jobs, leases, and the service loop land behind this boundary in
// later slices. The persistence boundary is shipped as a repository over
// storage::Pool so the eventual service has durable job/run state to consume.

#pragma once

#include <oran/automation/periodic.hpp>
#include <oran/automation/repository.hpp>
