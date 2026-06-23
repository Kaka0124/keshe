#ifndef GPU_SCHEDULING_MODELS_H
#define GPU_SCHEDULING_MODELS_H

#include <limits>

struct ServerSpec {
    int server_id;
    int gpu_count;
    int gpu_memory;   // per-GPU memory (GB)
    int cpu_cores;
    int memory;       // total RAM (GB)
};

struct Job {
    int job_id;
    int release_time;
    int duration;
    int min_gpu;
    int gpu_memory;   // total GPU memory needed (can be split across GPUs)
    int cpu_cores;
    int memory;       // RAM needed
    int weight;       // priority (higher = more important)
};

struct RunningJob {
    int job_id;
    int server_id;
    long long finish_time;
    int gpu_used;     // number of GPUs allocated
    int gpu_memory_used; // total GPU memory actually used
    int cpu_used;
    int memory_used;
};

struct ScheduleRecord {
    int job_id;
    int server_id;
    long long start_time;
    int gpu_used;
    long long finish_time;
};

// Per-time-slot resource snapshot for a single server
struct ServerSnapshot {
    int gpu_used_count;
    int gpu_memory_used;
    int cpu_used;
    int memory_used;
};

// Instance-level raw evaluation metrics
struct EvalMetrics {
    double wait_score;    // E_wait = sum(w_i * (t_i - r_i))
    double memory_score;  // E_memory = avg GPU memory idle per time slot
    long long finish_score; // E_finish = H = max(F_i)
};

constexpr long long INF_TIME = std::numeric_limits<long long>::max();

#endif
