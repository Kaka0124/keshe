#ifndef GPU_SCHEDULING_OPTIMIZER_H
#define GPU_SCHEDULING_OPTIMIZER_H

#include <chrono>
#include <vector>

#include "models.h"

// ============================================================
// Simulated Annealing optimizer
// ============================================================
//
// Operates on job ordering (permutation). Each perturbed ordering is
// fed through the greedy scheduler to produce a valid schedule.
// The greedy scheduler guarantees legality, so we never produce
// invalid solutions during SA.

class SAOptimizer {
public:
    struct Config {
        double initial_temp;
        double final_temp;
        double cooling_rate;
        int iterations_per_temp;
        long long time_budget_ms;
        int no_improve_limit;

        Config()
            : initial_temp(1000.0), final_temp(0.01), cooling_rate(0.9995),
              iterations_per_temp(50), time_budget_ms(50000),
              no_improve_limit(50000) {}
    };

    SAOptimizer(const std::vector<ServerSpec> &servers,
                const std::vector<Job> &jobs,
                const Config &cfg = Config());

    // Run SA starting from the given initial schedule.
    // Returns the best schedule found.
    std::vector<ScheduleRecord> optimize(
        const std::vector<ScheduleRecord> &initial_schedule
    );

private:
    // Compute a composite energy (lower = better) from metrics
    double energy(const EvalMetrics &m) const;

    // Build a valid schedule from a job ordering
    std::vector<ScheduleRecord> scheduleFromOrdering(
        const std::vector<int> &job_order
    );

    // Perturb job ordering: swap two random positions
    void perturb(std::vector<int> &order);

    const std::vector<ServerSpec> &servers;
    const std::vector<Job> &jobs;
    Config cfg;
    std::chrono::steady_clock::time_point start_time;

    // time since start in ms
    long long elapsedMs() const;
};

#endif
