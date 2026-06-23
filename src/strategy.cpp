#include "strategy.h"

#include <algorithm>
#include <cmath>
#include <climits>

#include "machine_state.h"

using namespace std;

// ============================================================
// Job comparators
// ============================================================

function<bool(const Job&, const Job&)> makeJobComparator(JobSortStrategy strategy) {
    switch (strategy) {
    case JobSortStrategy::BY_RELEASE:
        // Sort by release_time, then by priority density (mimics SA behavior)
        return [](const Job &a, const Job &b) {
            if (a.release_time != b.release_time) return a.release_time < b.release_time;
            double da = static_cast<double>(a.weight) / a.duration;
            double db = static_cast<double>(b.weight) / b.duration;
            if (da != db) return da > db;
            if (a.gpu_memory != b.gpu_memory) return a.gpu_memory < b.gpu_memory;
            return a.job_id < b.job_id;
        };

    case JobSortStrategy::BY_PRIORITY_DENSITY:
        // Sort by priority density: weight/duration (higher first)
        // Tie-break: release_time, then gpu_memory (smaller first to reduce fragmentation)
        return [](const Job &a, const Job &b) {
            double da = static_cast<double>(a.weight) / a.duration;
            double db = static_cast<double>(b.weight) / b.duration;
            if (da != db) return da > db;  // higher density first
            if (a.release_time != b.release_time) return a.release_time < b.release_time;
            if (a.gpu_memory != b.gpu_memory) return a.gpu_memory < b.gpu_memory;
            return a.job_id < b.job_id;
        };

    case JobSortStrategy::BY_RESOURCE_ASC:
        // Small resource jobs first -> better packing, fewer fragments
        return [](const Job &a, const Job &b) {
            int ra = a.min_gpu * 1000 + a.gpu_memory; // proxy for resource footprint
            int rb = b.min_gpu * 1000 + b.gpu_memory;
            if (ra != rb) return ra < rb;
            if (a.release_time != b.release_time) return a.release_time < b.release_time;
            return a.job_id < b.job_id;
        };

    case JobSortStrategy::BY_WEIGHT:
        return [](const Job &a, const Job &b) {
            if (a.weight != b.weight) return a.weight > b.weight; // higher weight first
            if (a.release_time != b.release_time) return a.release_time < b.release_time;
            if (a.duration != b.duration) return a.duration < b.duration;
            return a.job_id < b.job_id;
        };

    case JobSortStrategy::BY_SHORTEST_FIRST:
        return [](const Job &a, const Job &b) {
            if (a.duration != b.duration) return a.duration < b.duration;
            if (a.weight != b.weight) return a.weight > b.weight;
            if (a.release_time != b.release_time) return a.release_time < b.release_time;
            return a.job_id < b.job_id;
        };

    case JobSortStrategy::BY_GPU_MEM_FIT:
        // Best-fit approach: sort by GPU memory descending to handle large jobs first
        return [](const Job &a, const Job &b) {
            if (a.gpu_memory != b.gpu_memory) return a.gpu_memory > b.gpu_memory;
            if (a.min_gpu != b.min_gpu) return a.min_gpu > b.min_gpu;
            if (a.weight != b.weight) return a.weight > b.weight;
            if (a.release_time != b.release_time) return a.release_time < b.release_time;
            return a.job_id < b.job_id;
        };
    }

    // fallback
    return makeJobComparator(JobSortStrategy::BY_RELEASE);
}

// ============================================================
// Machine selection
// ============================================================

int selectMachine(
    MachineSelectStrategy strategy,
    const Job &job,
    const vector<const MachineState*> &machine_states,
    const vector<pair<int, int>> &feasible_entries
) {
    // Filter to currently feasible machines
    vector<pair<int, int>> feasible_now;  // (index, gpu_needed)
    for (const auto &entry : feasible_entries) {
        int idx = entry.first;
        int gpu_needed = entry.second;
        if (machine_states[idx]->canStart(job)) {
            feasible_now.push_back(entry);
        }
    }

    if (feasible_now.empty()) return -1;

    switch (strategy) {
    case MachineSelectStrategy::FIRST_FIT:
        return feasible_now[0].first;

    case MachineSelectStrategy::BEST_FIT_GPU: {
        // Pick machine with fewest remaining GPUs >= need (tight fit)
        int best_idx = -1;
        int best_remain = INT_MAX;
        for (const auto &entry : feasible_now) {
            int idx = entry.first;
            int gpu_needed = entry.second;
            int remain = machine_states[idx]->availableGpu() - gpu_needed;
            if (remain < best_remain) {
                best_remain = remain;
                best_idx = idx;
            }
        }
        return best_idx;
    }

    case MachineSelectStrategy::BEST_FIT_MEMORY: {
        // Pick machine with highest GPU memory utilization after placement
        int best_idx = -1;
        double best_util = -1.0;
        for (const auto &entry : feasible_now) {
            int idx = entry.first;
            long long total_mem = machine_states[idx]->totalGpuMemory();
            long long used_mem = machine_states[idx]->usedGpuMemory() + job.gpu_memory;
            double util = static_cast<double>(used_mem) / total_mem;
            if (util > best_util) {
                best_util = util;
                best_idx = idx;
            }
        }
        return best_idx;
    }

    case MachineSelectStrategy::WORST_FIT_GPU: {
        // Pick machine with most remaining GPUs (leave room for large jobs)
        int best_idx = -1;
        int best_remain = -1;
        for (const auto &entry : feasible_now) {
            int idx = entry.first;
            int remain = machine_states[idx]->availableGpu();
            if (remain > best_remain) {
                best_remain = remain;
                best_idx = idx;
            }
        }
        return best_idx;
    }

    case MachineSelectStrategy::LOAD_BALANCE: {
        // Pick machine with lowest load (fewest running jobs)
        int best_idx = -1;
        int best_load = INT_MAX;
        for (const auto &entry : feasible_now) {
            int idx = entry.first;
            // Load proxy: used GPU count
            int load = machine_states[idx]->spec.gpu_count - machine_states[idx]->availableGpu();
            if (load < best_load) {
                best_load = load;
                best_idx = idx;
            }
        }
        return best_idx;
    }

    case MachineSelectStrategy::BEST_FIT_COMBINED: {
        // Weighted score: GPU fit (0.5) + memory utilization (0.3) + load balance (0.2)
        int best_idx = -1;
        double best_score = -1e18;
        for (const auto &entry : feasible_now) {
            int idx = entry.first;
            int gpu_needed = entry.second;
            auto *ms = machine_states[idx];

            // GPU fit score: closer to exact fit is better
            int remain_gpu = ms->availableGpu() - gpu_needed;
            double gpu_score = 1.0 - static_cast<double>(remain_gpu) / ms->spec.gpu_count;

            // Memory utilization score
            long long total_mem = ms->totalGpuMemory();
            double mem_score = total_mem > 0
                ? static_cast<double>(ms->usedGpuMemory() + job.gpu_memory) / total_mem
                : 0.0;

            // Load score: prefer lightly loaded
            double load = static_cast<double>(ms->spec.gpu_count - ms->availableGpu()) / ms->spec.gpu_count;
            double load_score = 1.0 - load;

            double score = 0.5 * gpu_score + 0.3 * mem_score + 0.2 * load_score;
            if (score > best_score) {
                best_score = score;
                best_idx = idx;
            }
        }
        return best_idx;
    }
    }

    return -1;
}

// ============================================================
// Auto-select strategy
// ============================================================

StrategyConfig autoSelectConfig(
    int num_servers, int num_jobs,
    const vector<ServerSpec> &servers,
    const vector<Job> &jobs
) {
    StrategyConfig config;

    // Compute instance characteristics
    double job_per_server = static_cast<double>(num_jobs) / num_servers;

    int total_gpu = 0;
    int total_cpu = 0;
    long long total_mem = 0;
    for (const auto &s : servers) {
        total_gpu += s.gpu_count;
        total_cpu += s.cpu_cores;
        total_mem += s.memory;
    }

    // Check resource heterogeneity
    int min_gpu = INT_MAX, max_gpu = 0;
    for (const auto &s : servers) {
        min_gpu = min(min_gpu, s.gpu_count);
        max_gpu = max(max_gpu, s.gpu_count);
    }
    bool heterogeneous = (max_gpu - min_gpu >= 3);

    double avg_weight = 0;
    double avg_duration = 0;
    for (const auto &j : jobs) {
        avg_weight += j.weight;
        avg_duration += j.duration;
    }
    avg_weight /= num_jobs;
    avg_duration /= num_jobs;

    // Decision logic
    if (heterogeneous || max_gpu >= 6) {
        // Heterogeneous or high-GPU servers: use worst-fit to preserve large slots
        config.machine_select = MachineSelectStrategy::WORST_FIT_GPU;
    } else if (job_per_server > 50) {
        // High density: best-fit GPU to pack tightly
        config.machine_select = MachineSelectStrategy::BEST_FIT_COMBINED;
    } else {
        config.machine_select = MachineSelectStrategy::BEST_FIT_COMBINED;
    }

    // Job sorting: always use priority density as primary
    // For resource-tight instances, use resource-ascending
    if (job_per_server < 20 && avg_duration < 200) {
        config.job_sort = JobSortStrategy::BY_SHORTEST_FIRST;
    } else if (avg_weight > 10) {
        config.job_sort = JobSortStrategy::BY_PRIORITY_DENSITY;
    } else {
        config.job_sort = JobSortStrategy::BY_PRIORITY_DENSITY;
    }

    return config;
}

string jobSortStrategyName(JobSortStrategy s) {
    switch (s) {
    case JobSortStrategy::BY_RELEASE: return "BY_RELEASE";
    case JobSortStrategy::BY_PRIORITY_DENSITY: return "BY_PRIORITY_DENSITY";
    case JobSortStrategy::BY_RESOURCE_ASC: return "BY_RESOURCE_ASC";
    case JobSortStrategy::BY_WEIGHT: return "BY_WEIGHT";
    case JobSortStrategy::BY_SHORTEST_FIRST: return "BY_SHORTEST_FIRST";
    case JobSortStrategy::BY_GPU_MEM_FIT: return "BY_GPU_MEM_FIT";
    }
    return "UNKNOWN";
}

string machineSelectStrategyName(MachineSelectStrategy s) {
    switch (s) {
    case MachineSelectStrategy::FIRST_FIT: return "FIRST_FIT";
    case MachineSelectStrategy::BEST_FIT_GPU: return "BEST_FIT_GPU";
    case MachineSelectStrategy::BEST_FIT_MEMORY: return "BEST_FIT_MEMORY";
    case MachineSelectStrategy::WORST_FIT_GPU: return "WORST_FIT_GPU";
    case MachineSelectStrategy::LOAD_BALANCE: return "LOAD_BALANCE";
    case MachineSelectStrategy::BEST_FIT_COMBINED: return "BEST_FIT_COMBINED";
    }
    return "UNKNOWN";
}
