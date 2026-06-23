#ifndef GPU_SCHEDULING_MACHINE_STATE_H
#define GPU_SCHEDULING_MACHINE_STATE_H

#include <utility>
#include <vector>

#include "models.h"

class MachineState {
public:
    explicit MachineState(ServerSpec server);

    int requiredGpuCount(const Job &job) const;
    bool canEverRun(const Job &job) const;
    bool canStart(const Job &job) const;
    bool canStart(const Job &job, int gpu_used) const;
    std::pair<ScheduleRecord, RunningJob> startJob(const Job &job, long long current_time);
    std::pair<ScheduleRecord, RunningJob> startJob(const Job &job, long long current_time, int gpu_used);
    void releaseJob(const RunningJob &running_job);

    // Enhanced: get current resource availability
    int availableGpu() const { return remaining_gpu; }
    int availableCpu() const { return remaining_cpu; }
    int availableMemory() const { return remaining_memory; }
    long long totalGpuMemory() const { return static_cast<long long>(spec.gpu_count) * spec.gpu_memory; }
    long long usedGpuMemory() const { return gpu_memory_used; }
    long long idleGpuMemory() const { return totalGpuMemory() - gpu_memory_used; }

    ServerSpec spec;

private:
    int remaining_gpu = 0;
    int remaining_cpu = 0;
    int remaining_memory = 0;
    long long gpu_memory_used = 0;   // total GPU memory currently allocated
    std::vector<RunningJob> running_jobs;
};

#endif
