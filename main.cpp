#include <iostream>

#include "src/constraint_checker.h"
#include "src/models.h"
#include "src/optimizer.h"
#include "src/output.h"
#include "src/parser.h"
#include "src/scorer.h"
#include "src/scheduler.h"

using namespace std;

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    // --- Read input ---
    auto [servers, jobs] = readInstance(cin);
    if (jobs.empty()) return 0;

    int N = static_cast<int>(jobs.size());

    // --- Phase 1: Multi-strategy greedy for initial solution ---
    vector<ScheduleRecord> best_records = runMultiStrategy(servers, jobs);

    // --- Phase 2: SA improvement (time-budgeted) ---
    // Only run SA if instance is not trivial (N > 20)
    if (N > 20) {
        SAOptimizer::Config sa_cfg;

        // Keep SA light for testing; tune for production
        if (N <= 100) {
            sa_cfg.time_budget_ms = 500;     // 0.5s
            sa_cfg.iterations_per_temp = 50;
            sa_cfg.cooling_rate = 0.99;
        } else if (N <= 1000) {
            sa_cfg.time_budget_ms = 2000;    // 2s
            sa_cfg.iterations_per_temp = 30;
            sa_cfg.cooling_rate = 0.995;
        } else {
            sa_cfg.time_budget_ms = 5000;    // 5s
            sa_cfg.iterations_per_temp = 20;
            sa_cfg.cooling_rate = 0.997;
        }
        sa_cfg.no_improve_limit = 5000;

        EvalMetrics before_sa = computeMetrics(servers, jobs, best_records);

        SAOptimizer optimizer(servers, jobs, sa_cfg);
        auto sa_records = optimizer.optimize(best_records);

        if (!sa_records.empty()) {
            EvalMetrics after_sa = computeMetrics(servers, jobs, sa_records);
            // Accept SA result only if it improves (or is equal to) the greedy result
            double before_score = before_sa.wait_score * 0.01
                                + before_sa.memory_score
                                + before_sa.finish_score * 0.1;
            double after_score  = after_sa.wait_score * 0.01
                                + after_sa.memory_score
                                + after_sa.finish_score * 0.1;
            if (after_score <= before_score) {
                best_records = sa_records;
            }
        }
    }

    // --- Phase 3: Verify legality ---
    auto check_result = checkSchedule(servers, jobs, best_records);
    if (!check_result.valid) {
        // Fallback: use baseline scheduler (release-time sort, first-fit)
        // This guarantees a legal solution
        StrategyConfig fallback_cfg;
        fallback_cfg.job_sort = JobSortStrategy::BY_RELEASE;
        fallback_cfg.machine_select = MachineSelectStrategy::FIRST_FIT;

        GreedyScheduler fallback_scheduler(servers, jobs, fallback_cfg);
        best_records = fallback_scheduler.schedule();

        // Re-check
        auto fallback_check = checkSchedule(servers, jobs, best_records);
        if (!fallback_check.valid) {
            // Last resort: output nothing (will be caught by evaluator)
            // This should never happen per spec guarantee
            return 0;
        }
    }

    // --- Output ---
    writeScheduleRecords(cout, best_records);

    return 0;
}
