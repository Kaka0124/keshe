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

    // --- Phase 2: Metaheuristic improvement (time-budgeted) ---
    if (N > 20) {
        double best_score = 0;
        {
            auto m = computeMetrics(servers, jobs, best_records);
            best_score = m.wait_score * 0.01 + m.memory_score + m.finish_score * 0.1;
        }

        // ----- SA -----
        {
            SAOptimizer::Config sa_cfg;
            if (N <= 100) {
                sa_cfg.time_budget_ms = 5000;
                sa_cfg.iterations_per_temp = 100;
                sa_cfg.cooling_rate = 0.999;
                sa_cfg.initial_temp = 200.0;
            } else if (N <= 1000) {
                sa_cfg.time_budget_ms = 25000;
                sa_cfg.iterations_per_temp = 80;
                sa_cfg.cooling_rate = 0.9995;
                sa_cfg.initial_temp = 500.0;
            } else {
                // Hard/extreme: higher temp, slower cooling, 27s for SA
                sa_cfg.time_budget_ms = 27000;
                sa_cfg.iterations_per_temp = 80;
                sa_cfg.cooling_rate = 0.9998;
                sa_cfg.initial_temp = 2000.0;
            }
            sa_cfg.no_improve_limit = 100000;

            SAOptimizer sa_opt(servers, jobs, sa_cfg);
            auto sa_rec = sa_opt.optimize(best_records);
            if (!sa_rec.empty()) {
                auto m = computeMetrics(servers, jobs, sa_rec);
                double s = m.wait_score * 0.01 + m.memory_score + m.finish_score * 0.1;
                if (s < best_score) { best_score = s; best_records = sa_rec; }
            }
        }

        // ----- LAHC (parallel alternative for hard/extreme) -----
        if (N > 1000) {
            LAHCOptimizer::Config lahc_cfg;
            lahc_cfg.time_budget_ms = 27000;  // 27s for LAHC, total ~55s
            lahc_cfg.history_length = (N <= 3000) ? 1000 : 2000;
            lahc_cfg.no_improve_limit = 100000;

            LAHCOptimizer lahc_opt(servers, jobs, lahc_cfg);
            auto lahc_rec = lahc_opt.optimize(best_records);
            if (!lahc_rec.empty()) {
                auto m = computeMetrics(servers, jobs, lahc_rec);
                double s = m.wait_score * 0.01 + m.memory_score + m.finish_score * 0.1;
                if (s < best_score) { best_score = s; best_records = lahc_rec; }
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
