#ifndef GPU_SCHEDULING_SCORER_H
#define GPU_SCHEDULING_SCORER_H

#include <vector>

#include "models.h"

// Compute raw evaluation metrics for a schedule.
// Assumes the schedule is already verified as legal.
EvalMetrics computeMetrics(
    const std::vector<ServerSpec> &servers,
    const std::vector<Job> &jobs,
    const std::vector<ScheduleRecord> &records
);

#endif
