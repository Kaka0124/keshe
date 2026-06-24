#ifndef GPU_SCHEDULING_SCHEDULER_H
#define GPU_SCHEDULING_SCHEDULER_H

#include <queue>
#include <unordered_map>
#include <vector>

#include "machine_state.h"
#include "models.h"
#include "strategy.h"

struct FinishEvent {
    long long finish_time;
    int server_id;
    int job_id;
    RunningJob running_job;

    bool operator>(const FinishEvent &other) const;
};

class GreedyScheduler {
public:
    // Construct with explicit strategy
    GreedyScheduler(std::vector<ServerSpec> input_servers,
                    std::vector<Job> input_jobs,
                    StrategyConfig config);

    // Construct with auto-selected strategy
    GreedyScheduler(std::vector<ServerSpec> input_servers,
                    std::vector<Job> input_jobs);

    std::vector<ScheduleRecord> schedule();

    const StrategyConfig &getConfig() const { return config; }

private:
    struct StartResult {
        bool has_value = false;
        ScheduleRecord record{};
        RunningJob running_job{};
    };

    void buildFeasibleMachines();
    void releaseFinishedJobs(
        long long current_time,
        std::priority_queue<FinishEvent, std::vector<FinishEvent>,
                            std::greater<FinishEvent>> &running_heap
    );
    void tryStartPendingJobs(
        std::vector<Job> &pending_jobs,
        long long current_time,
        std::unordered_map<int, ScheduleRecord> &records,
        std::priority_queue<FinishEvent, std::vector<FinishEvent>,
                            std::greater<FinishEvent>> &running_heap
    );
    StartResult tryStartOneJob(const Job &job, long long current_time);
    long long nextEventTime(
        long long current_time,
        int next_job_index,
        const std::priority_queue<FinishEvent, std::vector<FinishEvent>,
                                  std::greater<FinishEvent>> &running_heap
    ) const;

    std::vector<ServerSpec> servers;
    std::vector<Job> jobs;
    StrategyConfig config;
    std::vector<MachineState> machines;
    std::unordered_map<int, int> machine_index_by_id;
    // feasible_machines[job_id] = list of (machine_index, gpu_needed)
    std::unordered_map<int, std::vector<std::pair<int, int>>> feasible_machines;
};

// ============================================================
// Multi-strategy runner: try multiple configs and return best
// ============================================================
std::vector<ScheduleRecord> runMultiStrategy(
    const std::vector<ServerSpec> &servers,
    const std::vector<Job> &jobs
);

// t25-style list scheduling: priority-greedy with earliest-start
std::vector<ScheduleRecord> listSchedule(
    const std::vector<ServerSpec> &servers,
    const std::vector<Job> &jobs
);

// Post-optimization: cross-server backfill + per-server compaction
std::vector<ScheduleRecord> postOptimize(
    const std::vector<ServerSpec> &servers,
    const std::vector<Job> &jobs,
    const std::vector<ScheduleRecord> &initial
);

#endif
