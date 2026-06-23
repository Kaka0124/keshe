#include "scorer.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_map>

#include "constraint_checker.h"
#include "parser.h"
#include "output.h"

using namespace std;

// ============================================================
// Raw metric computation (spec §二–§四)
// ============================================================

EvalMetrics computeMetrics(
    const vector<ServerSpec> &servers,
    const vector<Job> &jobs,
    const vector<ScheduleRecord> &records
) {
    unordered_map<int, Job> job_map;
    for (const auto &j : jobs) {
        job_map[j.job_id] = j;
    }

    // --- H = max finish time (§四) ---
    long long H = 0;
    for (const auto &rec : records) {
        if (rec.finish_time > H) H = rec.finish_time;
    }

    // --- E_wait = sum(w_i * (t_i - r_i)) (§二) ---
    double wait_score = 0.0;
    for (const auto &rec : records) {
        const Job &job = job_map[rec.job_id];
        wait_score += static_cast<double>(job.weight)
                    * (rec.start_time - job.release_time);
    }

    // --- E_memory = avg GPU memory idle per time slot (§三) ---
    // = (1/H) * sum_{t=0}^{H-1} sum_{s=1}^{M} (G_s * VG_s - sum_{i in A_s(t)} v_i)
    struct MemEvent {
        long long time;
        int server_id;
        long long delta_mem;
    };
    vector<MemEvent> events;
    events.reserve(records.size() * 2);

    for (const auto &rec : records) {
        const Job &job = job_map[rec.job_id];
        events.push_back({rec.start_time, rec.server_id,
                          static_cast<long long>(job.gpu_memory)});
        events.push_back({rec.finish_time, rec.server_id,
                          -static_cast<long long>(job.gpu_memory)});
    }

    sort(events.begin(), events.end(),
         [](const MemEvent &a, const MemEvent &b) { return a.time < b.time; });

    int M = static_cast<int>(servers.size());
    vector<long long> cur_mem(M + 1, 0);
    vector<long long> total_mem(M + 1, 0);
    for (const auto &s : servers) {
        total_mem[s.server_id] = static_cast<long long>(s.gpu_count) * s.gpu_memory;
    }

    long long cumulative_idle = 0;
    size_t ei = 0;
    long long prev_time = 0;

    while (ei < events.size()) {
        long long cur_time = events[ei].time;

        if (cur_time > prev_time) {
            long long slot_idle = 0;
            for (int s = 1; s <= M; ++s) {
                slot_idle += total_mem[s] - cur_mem[s];
            }
            cumulative_idle += slot_idle * (cur_time - prev_time);
        }

        while (ei < events.size() && events[ei].time == cur_time) {
            cur_mem[events[ei].server_id] += events[ei].delta_mem;
            ++ei;
        }
        prev_time = cur_time;
    }

    double memory_score = (H > 0) ? static_cast<double>(cumulative_idle) / H : 0.0;

    return {wait_score, memory_score, H};
}

// ============================================================
// Single-instance score (spec §五)
// ============================================================

double computeInstanceScore(
    const EvalMetrics &m,
    const InstanceBestWorst &bw
) {
    // Wait term
    double wait_term = 0.0;
    if (bw.worst_wait > bw.best_wait) {
        wait_term = (m.wait_score - bw.best_wait)
                  / (bw.worst_wait - bw.best_wait);
    }

    // Memory term
    double mem_term = 0.0;
    if (bw.worst_memory > bw.best_memory) {
        mem_term = (m.memory_score - bw.best_memory)
                 / (bw.worst_memory - bw.best_memory);
    }

    // Finish term
    double finish_term = 0.0;
    if (bw.worst_finish > bw.best_finish) {
        finish_term = static_cast<double>(m.finish_score - bw.best_finish)
                    / static_cast<double>(bw.worst_finish - bw.best_finish);
    }

    return wait_term + mem_term + finish_term;
}

// ============================================================
// Total score (spec §六)
// ============================================================

double computeTotalScore(const vector<double> &instance_scores) {
    if (instance_scores.empty()) return 3.0;
    double sum = 0.0;
    for (double s : instance_scores) sum += s;
    return sum / static_cast<double>(instance_scores.size());
}

// ============================================================
// All-in-one evaluation
// ============================================================

EvalResult evaluate(
    const string &input_text,
    const string &output_text,
    const InstanceBestWorst &bw
) {
    EvalResult result;

    // Parse input
    auto [servers, jobs] = parseInstance(input_text);

    // Parse output
    vector<ScheduleRecord> records;
    istringstream out_stream(output_text);
    string line;
    while (getline(out_stream, line)) {
        if (line.empty()) continue;
        istringstream ls(line);
        ScheduleRecord rec;
        if (!(ls >> rec.job_id >> rec.server_id
                  >> rec.start_time >> rec.gpu_used >> rec.finish_time)) {
            result.error_msg = "Malformed output line: " + line;
            return result;
        }
        records.push_back(rec);
    }

    // Check legality
    auto ck = checkSchedule(servers, jobs, records);
    if (!ck.valid) {
        result.error_msg = ck.error_msg;
        result.legal = false;
        result.instance_score = 3.0;
        return result;
    }

    result.legal = true;

    // Compute metrics and normalized score
    result.metrics = computeMetrics(servers, jobs, records);
    result.instance_score = computeInstanceScore(result.metrics, bw);

    return result;
}
