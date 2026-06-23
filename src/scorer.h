#ifndef GPU_SCHEDULING_SCORER_H
#define GPU_SCHEDULING_SCORER_H

#include <string>
#include <vector>

#include "models.h"

// ============================================================
// Raw metric computation (spec §二–§四)
// ============================================================

// Compute raw evaluation metrics for a schedule.
// Assumes the schedule is already verified as legal.
EvalMetrics computeMetrics(
    const std::vector<ServerSpec> &servers,
    const std::vector<Job> &jobs,
    const std::vector<ScheduleRecord> &records
);

// ============================================================
// Per-instance normalization (spec §五)
// ============================================================

// Best/worst values across all legal submissions for one instance.
struct InstanceBestWorst {
    double best_wait   = 0.0;
    double worst_wait  = 0.0;
    double best_memory = 0.0;
    double worst_memory = 0.0;
    long long best_finish  = 0;
    long long worst_finish = 0;
};

// Compute single-instance evaluation value E_{j,k}.
// Each term = (raw - best) / (worst - best).
// If worst == best for a term, that term is 0.
// Range: [0, 3], lower is better.
double computeInstanceScore(
    const EvalMetrics &metrics,
    const InstanceBestWorst &bw
);

// ============================================================
// Total score (spec §六)
// ============================================================

// E_j = average of all instance scores
double computeTotalScore(const std::vector<double> &instance_scores);

// ============================================================
// All-in-one evaluation from input + output files
// ============================================================

struct EvalResult {
    bool legal = false;
    std::string error_msg;
    EvalMetrics metrics{};
    double instance_score = 3.0;   // default: illegal = 3.0
};

// Parse an input file and output file, verify legality,
// then compute metrics and instance score.
EvalResult evaluate(
    const std::string &input_text,
    const std::string &output_text,
    const InstanceBestWorst &bw
);

#endif
