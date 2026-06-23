#include "constraint_checker.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

using namespace std;

CheckResult checkSchedule(
    const vector<ServerSpec> &servers,
    const vector<Job> &jobs,
    const vector<ScheduleRecord> &records
) {
    int N = static_cast<int>(jobs.size());
    int M = static_cast<int>(servers.size());

    // --- 1. Completeness & uniqueness ---
    if (static_cast<int>(records.size()) != N) {
        return {false, "Record count " + to_string(records.size()) +
                       " != job count " + to_string(N)};
    }

    unordered_set<int> seen_ids;
    for (const auto &rec : records) {
        if (rec.job_id < 1 || rec.job_id > N) {
            return {false, "Invalid job_id " + to_string(rec.job_id)};
        }
        if (seen_ids.count(rec.job_id)) {
            return {false, "Duplicate job_id " + to_string(rec.job_id)};
        }
        seen_ids.insert(rec.job_id);
    }

    // Build lookup maps
    unordered_map<int, Job> job_map;
    for (const auto &j : jobs) {
        job_map[j.job_id] = j;
    }
    unordered_map<int, ServerSpec> server_map;
    for (const auto &s : servers) {
        server_map[s.server_id] = s;
    }

    // Find H = max finish time
    long long H = 0;
    for (const auto &rec : records) {
        if (rec.finish_time > H) H = rec.finish_time;
    }

    // Per-server timeline data structure:
    // For each time slot t, track resource usage per server
    // Use an event-based approach: for each job, add/remove resources at start/end

    // Store intervals: (time, server_id, delta_gpu, delta_cpu, delta_mem)
    struct Event {
        long long time;
        int server_id;
        int d_gpu;
        int d_cpu;
        int d_mem;
    };
    vector<Event> events;
    events.reserve(records.size() * 2);

    for (const auto &rec : records) {
        const Job &job = job_map[rec.job_id];

        // --- 2. Release time constraint ---
        if (rec.start_time < job.release_time) {
            return {false, "Job " + to_string(rec.job_id) +
                           " starts at " + to_string(rec.start_time) +
                           " before release " + to_string(job.release_time)};
        }

        // --- 4. Finish time consistency ---
        if (rec.finish_time != rec.start_time + job.duration) {
            return {false, "Job " + to_string(rec.job_id) +
                           " finish_time " + to_string(rec.finish_time) +
                           " != start_time " + to_string(rec.start_time) +
                           " + duration " + to_string(job.duration)};
        }

        // --- 5. Job-server matching constraints ---
        const ServerSpec &srv = server_map[rec.server_id];

        if (rec.gpu_used < job.min_gpu) {
            return {false, "Job " + to_string(rec.job_id) +
                           " GPU count " + to_string(rec.gpu_used) +
                           " < min " + to_string(job.min_gpu)};
        }
        if (rec.gpu_used > srv.gpu_count) {
            return {false, "Job " + to_string(rec.job_id) +
                           " GPU count " + to_string(rec.gpu_used) +
                           " > server " + to_string(rec.server_id) +
                           " GPU count " + to_string(srv.gpu_count)};
        }
        if (job.gpu_memory > static_cast<long long>(rec.gpu_used) * srv.gpu_memory) {
            return {false, "Job " + to_string(rec.job_id) +
                           " GPU memory " + to_string(job.gpu_memory) +
                           " > " + to_string(rec.gpu_used) + " x " +
                           to_string(srv.gpu_memory)};
        }
        if (job.cpu_cores > srv.cpu_cores) {
            return {false, "Job " + to_string(rec.job_id) +
                           " CPU " + to_string(job.cpu_cores) +
                           " > server " + to_string(rec.server_id) +
                           " CPU " + to_string(srv.cpu_cores)};
        }
        if (job.memory > srv.memory) {
            return {false, "Job " + to_string(rec.job_id) +
                           " memory " + to_string(job.memory) +
                           " > server " + to_string(rec.server_id) +
                           " memory " + to_string(srv.memory)};
        }

        // --- 3. Running process constraint (implicit: no preemption in our model) ---
        // Since we assign start_time + duration = finish_time, continuity is guaranteed
        // as long as resources are not over-committed at any point. That check is below.

        // Record resource usage events
        events.push_back({rec.start_time, rec.server_id,
                          rec.gpu_used, job.cpu_cores, job.memory});
        events.push_back({rec.finish_time, rec.server_id,
                          -rec.gpu_used, -job.cpu_cores, -job.memory});
    }

    // --- 6. Server concurrency constraint ---
    // Sort events by time, process in order
    sort(events.begin(), events.end(), [](const Event &a, const Event &b) {
        return a.time < b.time;
    });

    // Track current usage per server
    vector<int> cur_gpu(M + 1, 0);
    vector<int> cur_cpu(M + 1, 0);
    vector<int> cur_mem(M + 1, 0);

    size_t ei = 0;
    while (ei < events.size()) {
        long long cur_time = events[ei].time;

        // Apply all events at this time
        while (ei < events.size() && events[ei].time == cur_time) {
            const Event &ev = events[ei];
            cur_gpu[ev.server_id] += ev.d_gpu;
            cur_cpu[ev.server_id] += ev.d_cpu;
            cur_mem[ev.server_id] += ev.d_mem;
            ++ei;
        }

        // Check limits
        for (int s = 1; s <= M; ++s) {
            const ServerSpec &srv = server_map[s];
            if (cur_gpu[s] > srv.gpu_count) {
                return {false, "Time " + to_string(cur_time) +
                               " server " + to_string(s) +
                               " GPU " + to_string(cur_gpu[s]) +
                               " > " + to_string(srv.gpu_count)};
            }
            if (cur_cpu[s] > srv.cpu_cores) {
                return {false, "Time " + to_string(cur_time) +
                               " server " + to_string(s) +
                               " CPU " + to_string(cur_cpu[s]) +
                               " > " + to_string(srv.cpu_cores)};
            }
            if (cur_mem[s] > srv.memory) {
                return {false, "Time " + to_string(cur_time) +
                               " server " + to_string(s) +
                               " memory " + to_string(cur_mem[s]) +
                               " > " + to_string(srv.memory)};
            }
        }
    }

    return {true, ""};
}
