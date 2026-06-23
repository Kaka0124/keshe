#ifndef GPU_SCHEDULING_CONSTRAINT_CHECKER_H
#define GPU_SCHEDULING_CONSTRAINT_CHECKER_H

#include <string>
#include <vector>

#include "models.h"

struct CheckResult {
    bool valid = true;
    std::string error_msg;
};

// Verify that a schedule satisfies ALL constraints defined in the spec.
// Returns the first violation found, or valid=true if all constraints pass.
CheckResult checkSchedule(
    const std::vector<ServerSpec> &servers,
    const std::vector<Job> &jobs,
    const std::vector<ScheduleRecord> &records
);

#endif
