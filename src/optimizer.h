#ifndef GPU_SCHEDULING_OPTIMIZER_H
#define GPU_SCHEDULING_OPTIMIZER_H

#include <chrono>
#include <vector>

#include "models.h"

// ============================================================
// Simulated Annealing optimizer
// ============================================================
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

    std::vector<ScheduleRecord> optimize(
        const std::vector<ScheduleRecord> &initial_schedule
    );

private:
    double energy(const EvalMetrics &m) const;
    std::vector<ScheduleRecord> scheduleFromOrdering(const std::vector<int> &job_order);
    void perturb(std::vector<int> &order);

    const std::vector<ServerSpec> &servers;
    const std::vector<Job> &jobs;
    Config cfg;
    std::chrono::steady_clock::time_point start_time;
    long long elapsedMs() const;
};

// ============================================================
// Late Acceptance Hill Climbing optimizer
// ============================================================
//
// Parameter-free alternative to SA. Maintains a history of past
// objective values. Accepts a move if it beats the value from
// L iterations ago. Works well on hard/extreme cases where
// SA temperature tuning is difficult.
class LAHCOptimizer {
public:
    struct Config {
        int history_length;
        long long time_budget_ms;
        int no_improve_limit;

        Config()
            : history_length(500), time_budget_ms(50000),
              no_improve_limit(50000) {}
    };

    LAHCOptimizer(const std::vector<ServerSpec> &servers,
                  const std::vector<Job> &jobs,
                  const Config &cfg = Config());

    std::vector<ScheduleRecord> optimize(
        const std::vector<ScheduleRecord> &initial_schedule
    );

private:
    double energy(const EvalMetrics &m) const;
    std::vector<ScheduleRecord> scheduleFromOrdering(const std::vector<int> &job_order);
    void perturb(std::vector<int> &order);

    const std::vector<ServerSpec> &servers;
    const std::vector<Job> &jobs;
    Config cfg;
    std::chrono::steady_clock::time_point start_time;
    long long elapsedMs() const;
};

#endif
