#include <chrono>
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

    // --- Phase 1: Build initial solution ---
    // N<=3000: t25-style list scheduling (fastest, best quality for medium cases)
    // N>3000: multi-strategy greedy (listSchedule earliest_start is too slow per job)
    vector<ScheduleRecord> best_records;
    if (N <= 3000) {
        best_records = listSchedule(servers, jobs);
        best_records = postOptimize(servers, jobs, best_records);
        // Also try multi-strategy — keep whichever is better
        auto multi = runMultiStrategy(servers, jobs);
        multi = postOptimize(servers, jobs, multi);
        auto ml = computeMetrics(servers, jobs, best_records);
        auto mm = computeMetrics(servers, jobs, multi);
        double sl = ml.wait_score * 0.01 + ml.memory_score + ml.finish_score * 0.1;
        double sm = mm.wait_score * 0.01 + mm.memory_score + mm.finish_score * 0.1;
        if (sm < sl) best_records = multi;
    } else {
        // N>3000: postOptimize backfill is too slow (earliest_start per job)
        // and empirically doesn't help on these cases. Rely on SA alone.
        best_records = runMultiStrategy(servers, jobs);
    }

    // Save the greedy baseline so we can always fall back to it
    const vector<ScheduleRecord> greedy_baseline = best_records;
    double greedy_score = 0;
    {
        auto m = computeMetrics(servers, jobs, greedy_baseline);
        greedy_score = m.wait_score * 0.01 + m.memory_score + m.finish_score * 0.1;
    }

    // --- Phase 2: Metaheuristic improvement (time-budgeted) ---
    if (N > 20) {
        double best_score = greedy_score;

        if (N <= 2000) {
            // ---- Full SA + LAHC for N ≤ 2000 (target < 50s) ----
            {
                SAOptimizer::Config sa_cfg;
                if (N <= 100) {
                    sa_cfg.time_budget_ms = 5000;
                    sa_cfg.iterations_per_temp = 100;
                    sa_cfg.cooling_rate = 0.999;
                    sa_cfg.initial_temp = 200.0;
                } else if (N <= 1000) {
                    sa_cfg.time_budget_ms = 20000;
                    sa_cfg.iterations_per_temp = 50;
                    sa_cfg.cooling_rate = 0.9995;
                    sa_cfg.initial_temp = 500.0;
                } else {
                    sa_cfg.time_budget_ms = 20000;
                    sa_cfg.iterations_per_temp = 30;
                    sa_cfg.cooling_rate = 0.9995;
                    sa_cfg.initial_temp = 1000.0;
                }
                sa_cfg.no_improve_limit = 50000;
                SAOptimizer sa_opt(servers, jobs, sa_cfg);
                auto sa_rec = sa_opt.optimize(best_records);
                if (!sa_rec.empty()) {
                    auto m = computeMetrics(servers, jobs, sa_rec);
                    double s = m.wait_score * 0.01 + m.memory_score + m.finish_score * 0.1;
                    if (s < best_score) { best_score = s; best_records = sa_rec; }
                }
            }
            if (N > 100) {
                LAHCOptimizer::Config lahc_cfg;
                lahc_cfg.time_budget_ms = (N <= 1000) ? 8000 : 15000;
                lahc_cfg.history_length = (N <= 1000) ? 500 : 1000;
                lahc_cfg.no_improve_limit = 50000;
                LAHCOptimizer lahc_opt(servers, jobs, lahc_cfg);
                auto lahc_rec = lahc_opt.optimize(best_records);
                if (!lahc_rec.empty()) {
                    auto m = computeMetrics(servers, jobs, lahc_rec);
                    double s = m.wait_score * 0.01 + m.memory_score + m.finish_score * 0.1;
                    if (s < best_score) { best_score = s; best_records = lahc_rec; }
                }
            }
        } else {
            // ---- SA for N > 2000 (20s, high-temp slow-cool, target < 40s) ----
            {
                SAOptimizer::Config sa_cfg;
                sa_cfg.time_budget_ms = 20000;
                sa_cfg.iterations_per_temp = 20;
                sa_cfg.cooling_rate = 0.999;
                sa_cfg.initial_temp = 2000.0;
                sa_cfg.no_improve_limit = 50000;
                SAOptimizer sa_opt(servers, jobs, sa_cfg);
                auto sa_rec = sa_opt.optimize(best_records);
                if (!sa_rec.empty()) {
                    auto m = computeMetrics(servers, jobs, sa_rec);
                    double s = m.wait_score * 0.01 + m.memory_score + m.finish_score * 0.1;
                    if (s < best_score) { best_score = s; best_records = sa_rec; }
                }
            }
        }
    }

    // --- Final safeguard ---
    {
        auto m = computeMetrics(servers, jobs, best_records);
        double final_score = m.wait_score * 0.01 + m.memory_score + m.finish_score * 0.1;
        if (final_score > greedy_score) {
            best_records = greedy_baseline;
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
