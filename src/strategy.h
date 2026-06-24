#ifndef GPU_SCHEDULING_STRATEGY_H
#define GPU_SCHEDULING_STRATEGY_H

#include <functional>
#include <string>
#include <vector>

#include "models.h"

// ============================================================
// Job sorting strategies
// ============================================================
enum class JobSortStrategy {
    BY_RELEASE,              // baseline: (release_time, duration, job_id)
    BY_PRIORITY_DENSITY,     // high w/p first: (-w/d, r, g)
    BY_WEIGHT_DENSITY_REL,   // high w/(r+p+1) first: t25-style
    BY_RESOURCE_ASC,         // small jobs first: (min_gpu, gpu_memory, r)
    BY_WEIGHT,          // high weight first: (-w, r, d)
    BY_SHORTEST_FIRST,  // short duration first: (d, -w, r)
    BY_GPU_MEM_FIT,     // best GPU memory fit: (gpu_memory, d, -w)
};

// ============================================================
// Machine selection strategies
// ============================================================
enum class MachineSelectStrategy {
    FIRST_FIT,          // baseline: first feasible machine
    BEST_FIT_GPU,       // pick machine with fewest remaining GPUs >= need
    BEST_FIT_MEMORY,    // pick machine with highest GPU memory utilization after placement
    WORST_FIT_GPU,      // pick machine with most remaining GPUs (reserve for large jobs)
    LOAD_BALANCE,       // pick machine with lowest current load
    BEST_FIT_COMBINED,  // weighted combination of GPU fit + load
};

// ============================================================
// Strategy configuration
// ============================================================
struct StrategyConfig {
    JobSortStrategy job_sort = JobSortStrategy::BY_PRIORITY_DENSITY;
    MachineSelectStrategy machine_select = MachineSelectStrategy::BEST_FIT_COMBINED;
    bool enable_backtrack = false;   // try alternate machines if first choice blocks
    bool use_fallback = true;        // fallback to first-fit on failure
};

// ============================================================
// Strategy functions
// ============================================================

// Returns a comparator for jobs based on the given strategy
std::function<bool(const Job&, const Job&)> makeJobComparator(JobSortStrategy strategy);

// Returns the index of the best machine to schedule the given job on.
// machine_states: vector of MachineState pointers (for polymorphism-free lookup)
// feasible_map: for each machine index, the required GPU count (or -1 if infeasible)
// Returns -1 if no feasible machine exists.
int selectMachine(
    MachineSelectStrategy strategy,
    const Job &job,
    const std::vector<const class MachineState*> &machine_states,
    const std::vector<std::pair<int, int>> &feasible_entries  // (index, gpu_needed) pairs
);

// Auto-select best strategy config based on instance characteristics
StrategyConfig autoSelectConfig(int num_servers, int num_jobs,
                                 const std::vector<ServerSpec> &servers,
                                 const std::vector<Job> &jobs);

std::string jobSortStrategyName(JobSortStrategy s);
std::string machineSelectStrategyName(MachineSelectStrategy s);

#endif
