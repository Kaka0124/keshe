#include "scorer.h"

#include <algorithm>
#include <unordered_map>

using namespace std;

EvalMetrics computeMetrics(
    const vector<ServerSpec> &servers,
    const vector<Job> &jobs,
    const vector<ScheduleRecord> &records
) {
    // Build job lookup
    unordered_map<int, Job> job_map;
    for (const auto &j : jobs) {
        job_map[j.job_id] = j;
    }

    // Find H = max finish time
    long long H = 0;
    for (const auto &rec : records) {
        if (rec.finish_time > H) H = rec.finish_time;
    }

    // E_wait = sum(w_i * (t_i - r_i))
    double wait_score = 0.0;
    for (const auto &rec : records) {
        const Job &job = job_map[rec.job_id];
        wait_score += static_cast<double>(job.weight) * (rec.start_time - job.release_time);
    }

    // E_memory = avg GPU memory idle per time slot
    // For each time slot 0..H-1, for each server: idle = total_mem - used_mem
    // Use sweep-line by time
    struct MemEvent {
        long long time;
        int server_id;
        long long delta_mem; // GPU memory delta
    };
    vector<MemEvent> events;
    events.reserve(records.size() * 2);

    for (const auto &rec : records) {
        const Job &job = job_map[rec.job_id];
        events.push_back({rec.start_time, rec.server_id, static_cast<long long>(job.gpu_memory)});
        events.push_back({rec.finish_time, rec.server_id, -static_cast<long long>(job.gpu_memory)});
    }

    sort(events.begin(), events.end(), [](const MemEvent &a, const MemEvent &b) {
        return a.time < b.time;
    });

    int M = static_cast<int>(servers.size());
    vector<long long> cur_mem(M + 1, 0);
    vector<long long> total_mem(M + 1, 0);
    for (const auto &s : servers) {
        total_mem[s.server_id] = static_cast<long long>(s.gpu_count) * s.gpu_memory;
    }

    // Compute per-time-slot total idle memory
    long long cumulative_idle = 0;
    size_t ei = 0;
    long long prev_time = 0;

    while (ei < events.size()) {
        long long cur_time = events[ei].time;

        // Add idle for interval [prev_time, cur_time)
        if (cur_time > prev_time) {
            long long slot_idle = 0;
            for (int s = 1; s <= M; ++s) {
                slot_idle += total_mem[s] - cur_mem[s];
            }
            cumulative_idle += slot_idle * (cur_time - prev_time);
        }

        // Apply all events at this time
        while (ei < events.size() && events[ei].time == cur_time) {
            cur_mem[events[ei].server_id] += events[ei].delta_mem;
            ++ei;
        }
        prev_time = cur_time;
    }

    double memory_score = (H > 0) ? static_cast<double>(cumulative_idle) / H : 0.0;

    return {wait_score, memory_score, H};
}
