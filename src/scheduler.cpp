#include "scheduler.h"

#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <unordered_map>

#include "machine_state.h"
#include "scorer.h"
#include "timeline.h"

using namespace std;

// ============================================================
// FinishEvent
// ============================================================
bool FinishEvent::operator>(const FinishEvent &other) const {
    if (finish_time != other.finish_time) return finish_time > other.finish_time;
    if (server_id != other.server_id) return server_id > other.server_id;
    return job_id > other.job_id;
}

// ============================================================
// GreedyScheduler
// ============================================================

GreedyScheduler::GreedyScheduler(
    vector<ServerSpec> input_servers,
    vector<Job> input_jobs,
    StrategyConfig cfg
) : servers(std::move(input_servers)), jobs(std::move(input_jobs)), config(cfg) {

    // Sort servers by ID
    sort(servers.begin(), servers.end(),
         [](const ServerSpec &a, const ServerSpec &b) { return a.server_id < b.server_id; });

    // Sort jobs by selected strategy
    auto comp = makeJobComparator(config.job_sort);
    sort(jobs.begin(), jobs.end(), comp);

    // Create machines
    for (const auto &server : servers) {
        machines.emplace_back(server);
    }
    for (int i = 0; i < static_cast<int>(machines.size()); ++i) {
        machine_index_by_id[machines[i].spec.server_id] = i;
    }

    buildFeasibleMachines();
}

GreedyScheduler::GreedyScheduler(
    vector<ServerSpec> input_servers,
    vector<Job> input_jobs
) : GreedyScheduler(std::move(input_servers), std::move(input_jobs),
                    autoSelectConfig(input_servers.size(), input_jobs.size(),
                                     input_servers, input_jobs)) {}

void GreedyScheduler::buildFeasibleMachines() {
    for (const auto &job : jobs) {
        vector<pair<int, int>> entries;
        for (int i = 0; i < static_cast<int>(machines.size()); ++i) {
            if (machines[i].canEverRun(job)) {
                int gpu_needed = machines[i].requiredGpuCount(job);
                entries.push_back({i, gpu_needed});
            }
        }
        if (entries.empty()) {
            throw runtime_error("Job " + to_string(job.job_id) +
                                " cannot run on any server (should never happen per spec guarantee).");
        }
        feasible_machines[job.job_id] = entries;
    }
}

vector<ScheduleRecord> GreedyScheduler::schedule() {
    if (jobs.empty()) return {};

    long long current_time = jobs[0].release_time;
    int next_job_index = 0;
    unordered_map<int, ScheduleRecord> records;
    priority_queue<FinishEvent, vector<FinishEvent>, greater<FinishEvent>> running_heap;
    vector<Job> pending_jobs;  // persists across iterations

    while (static_cast<int>(records.size()) < static_cast<int>(jobs.size())) {
        releaseFinishedJobs(current_time, running_heap);

        // Collect newly released jobs and append to existing pending list
        while (next_job_index < static_cast<int>(jobs.size()) &&
               jobs[next_job_index].release_time <= current_time) {
            pending_jobs.push_back(jobs[next_job_index]);
            ++next_job_index;
        }

        // Sort pending jobs by strategy (new arrivals + previously unscheduled)
        auto comp = makeJobComparator(config.job_sort);
        sort(pending_jobs.begin(), pending_jobs.end(), comp);

        tryStartPendingJobs(pending_jobs, current_time, records, running_heap);
        // pending_jobs now contains only unscheduled jobs (tryStartPendingJobs
        // replaces it with still_pending)

        if (static_cast<int>(records.size()) == static_cast<int>(jobs.size())) {
            break;
        }

        current_time = nextEventTime(current_time, next_job_index, running_heap);
    }

    // Build ordered output
    vector<ScheduleRecord> ordered;
    ordered.reserve(records.size());
    for (int job_id = 1; job_id <= static_cast<int>(jobs.size()); ++job_id) {
        auto it = records.find(job_id);
        if (it != records.end()) {
            ordered.push_back(it->second);
        }
    }
    return ordered;
}

void GreedyScheduler::releaseFinishedJobs(
    long long current_time,
    priority_queue<FinishEvent, vector<FinishEvent>, greater<FinishEvent>> &running_heap
) {
    while (!running_heap.empty() && running_heap.top().finish_time <= current_time) {
        FinishEvent event = running_heap.top();
        running_heap.pop();
        int idx = machine_index_by_id.at(event.server_id);
        machines[idx].releaseJob(event.running_job);
    }
}

void GreedyScheduler::tryStartPendingJobs(
    vector<Job> &pending_jobs,
    long long current_time,
    unordered_map<int, ScheduleRecord> &records,
    priority_queue<FinishEvent, vector<FinishEvent>, greater<FinishEvent>> &running_heap
) {
    // Repeatedly try to schedule from pending list until no job can be scheduled
    bool changed = true;
    while (changed && !pending_jobs.empty()) {
        changed = false;
        vector<Job> still_pending;

        for (auto &job : pending_jobs) {
            auto result = tryStartOneJob(job, current_time);
            if (result.has_value) {
                records[job.job_id] = result.record;
                running_heap.push(FinishEvent{
                    result.running_job.finish_time,
                    result.running_job.server_id,
                    result.running_job.job_id,
                    result.running_job,
                });
                changed = true;
            } else {
                still_pending.push_back(job);
            }
        }
        pending_jobs = std::move(still_pending);
    }
}

GreedyScheduler::StartResult GreedyScheduler::tryStartOneJob(
    const Job &job, long long current_time
) {
    const auto &entries = feasible_machines.at(job.job_id);

    // Use original first-fit logic for the baseline strategy to ensure correctness
    if (config.machine_select == MachineSelectStrategy::FIRST_FIT) {
        for (const auto &entry : entries) {
            int machine_index = entry.first;
            int gpu_used = entry.second;
            if (machines[machine_index].canStart(job, gpu_used)) {
                auto [record, running_job] = machines[machine_index].startJob(
                    job, current_time, gpu_used);
                return StartResult{true, record, running_job};
            }
        }
        return StartResult{};
    }

    // Enhanced selection: use strategy-based machine picking
    vector<const MachineState*> machine_ptrs;
    machine_ptrs.reserve(machines.size());
    for (const auto &m : machines) {
        machine_ptrs.push_back(&m);
    }

    int chosen = selectMachine(config.machine_select, job, machine_ptrs, entries);
    if (chosen < 0) return StartResult{};

    int gpu_needed = -1;
    for (const auto &entry : entries) {
        if (entry.first == chosen) {
            gpu_needed = entry.second;
            break;
        }
    }

    auto [record, running_job] = machines[chosen].startJob(job, current_time, gpu_needed);
    return StartResult{true, record, running_job};
}

long long GreedyScheduler::nextEventTime(
    long long current_time,
    int next_job_index,
    const priority_queue<FinishEvent, vector<FinishEvent>, greater<FinishEvent>> &running_heap
) const {
    long long next_time = INF_TIME;

    if (next_job_index < static_cast<int>(jobs.size())) {
        next_time = min(next_time, static_cast<long long>(jobs[next_job_index].release_time));
    }
    if (!running_heap.empty()) {
        next_time = min(next_time, running_heap.top().finish_time);
    }

    if (next_time == INF_TIME) {
        throw runtime_error("No future event exists — remaining jobs may be infeasible.");
    }

    return next_time;
}

// ============================================================
// Multi-strategy runner
// ============================================================

vector<ScheduleRecord> runMultiStrategy(
    const vector<ServerSpec> &servers,
    const vector<Job> &jobs
) {
    struct Combo {
        JobSortStrategy js;
        MachineSelectStrategy ms;
    };

    // Curated strategy combos — more is not always better.
    // These 10 combos cover the most promising search directions.
    // 10 curated strategy combos.
    // For N>3000 each greedy run costs ~1.5s, so 10 combos = ~15s.
    // This is acceptable as long as subsequent SA is budgeted accordingly.
    vector<Combo> combos = {
        {JobSortStrategy::BY_PRIORITY_DENSITY, MachineSelectStrategy::BEST_FIT_COMBINED},
        {JobSortStrategy::BY_PRIORITY_DENSITY, MachineSelectStrategy::BEST_FIT_GPU},
        {JobSortStrategy::BY_PRIORITY_DENSITY, MachineSelectStrategy::WORST_FIT_GPU},
        {JobSortStrategy::BY_PRIORITY_DENSITY, MachineSelectStrategy::LOAD_BALANCE},
        {JobSortStrategy::BY_WEIGHT_DENSITY_REL, MachineSelectStrategy::BEST_FIT_COMBINED},
        {JobSortStrategy::BY_WEIGHT,           MachineSelectStrategy::BEST_FIT_COMBINED},
        {JobSortStrategy::BY_WEIGHT,           MachineSelectStrategy::BEST_FIT_GPU},
        {JobSortStrategy::BY_RESOURCE_ASC,     MachineSelectStrategy::BEST_FIT_COMBINED},
        {JobSortStrategy::BY_SHORTEST_FIRST,   MachineSelectStrategy::BEST_FIT_COMBINED},
        {JobSortStrategy::BY_GPU_MEM_FIT,      MachineSelectStrategy::WORST_FIT_GPU},
        {JobSortStrategy::BY_RELEASE,          MachineSelectStrategy::FIRST_FIT},
    };

    vector<ScheduleRecord> best_records;
    double best_score = 1e18;

    for (const auto &combo : combos) {
        StrategyConfig cfg;
        cfg.job_sort = combo.js;
        cfg.machine_select = combo.ms;

        try {
            GreedyScheduler scheduler(servers, jobs, cfg);
            auto records = scheduler.schedule();

            EvalMetrics metrics = computeMetrics(servers, jobs, records);
            // OFFICIAL scoring weights: must match SA energy function
            double score = metrics.wait_score * 0.01 + metrics.memory_score + metrics.finish_score * 0.1;

            if (score < best_score || best_records.empty()) {
                best_score = score;
                best_records = records;
            }
        } catch (const exception &) {
            // Skip failed strategies
        }
    }

    return best_records;
}

// ============================================================
// t25-style list scheduling: per-job earliest-start placement
// ============================================================
// Orders jobs by weight/(r+p+1), then places each at its earliest
// feasible start time across all servers. This matches team25's
// Phase 1 and produces much better initial solutions than the
// event-driven GreedyScheduler for medium/large cases.

vector<ScheduleRecord> listSchedule(
    const vector<ServerSpec> &servers,
    const vector<Job> &jobs
) {
    int M = (int)servers.size();
    int N = (int)jobs.size();

    // Build job vector sorted by weight/(r+p+1) (t25 ordering)
    vector<int> order(N);
    for (int i = 0; i < N; i++) order[i] = i;
    sort(order.begin(), order.end(), [&](int a, int b) {
        const Job &ja = jobs[a], &jb = jobs[b];
        double sa = (double)ja.weight / (ja.release_time + ja.duration + 1);
        double sb = (double)jb.weight / (jb.release_time + jb.duration + 1);
        if (fabs(sa - sb) > 1e-12) return sa > sb;
        if (ja.release_time != jb.release_time) return ja.release_time < jb.release_time;
        return ja.duration < jb.duration;
    });

    // Pre-compute feasible servers per job
    // (server_id, min_gpu_count)
    vector<vector<pair<int,int>>> feasible(N);
    for (int i = 0; i < N; i++) {
        const Job &j = jobs[i];
        for (const auto &s : servers) {
            if (j.cpu_cores > s.cpu_cores || j.memory > s.memory) continue;
            int u_mem = (j.gpu_memory + s.gpu_memory - 1) / s.gpu_memory;
            int u = max(j.min_gpu, u_mem);
            if (u <= s.gpu_count)
                feasible[i].push_back({s.server_id, u});
        }
    }

    // Initialize server timelines
    vector<ServerTimeline> timelines(M + 1);
    for (const auto &s : servers)
        timelines[s.server_id].init(s.gpu_count, s.gpu_memory,
                                     s.cpu_cores, s.memory);

    // Build server lookup
    unordered_map<int, const ServerSpec*> srv_map;
    for (const auto &s : servers) srv_map[s.server_id] = &s;

    vector<ScheduleRecord> result(N + 1); // 1-indexed
    int max_iter = (N <= 1000) ? 500 : 200;

    for (int idx : order) {
        const Job &j = jobs[idx];
        double best_cost = 1e30;
        int best_s = -1, best_u = 0;
        long long best_t = 0;

        for (auto &[sid, u_min] : feasible[idx]) {
            const ServerSpec &sv = *srv_map[sid];
            for (int u = u_min; u <= min(u_min + 1, sv.gpu_count); u++) {
                if ((long long)u * sv.gpu_memory < j.gpu_memory) continue;
                long long t = timelines[sid].earliestStart(
                    j.release_time, j.duration, u,
                    j.cpu_cores, j.memory, max_iter);
                if (t < 0) continue;
                double cost = (double)j.weight * (t - j.release_time)
                    + (double)((long long)u * sv.gpu_memory - j.gpu_memory) * 5.0
                    + (double)(t + j.duration) * 2.0;
                if (cost < best_cost - 1e-12)
                    best_cost = cost, best_s = sid, best_t = t, best_u = u;
            }
        }

        // Fallback: linear scan on first feasible server
        if (best_s < 0) {
            for (auto &[sid, u] : feasible[idx]) {
                for (long long t = j.release_time; t <= j.release_time + 500000; t++) {
                    if (timelines[sid].canStart(t, j.duration, u,
                                                 j.cpu_cores, j.memory)) {
                        best_s = sid; best_t = t; best_u = u; break;
                    }
                }
                if (best_s >= 0) break;
            }
        }

        // Absolute fallback
        if (best_s < 0) {
            auto &[sid, u] = feasible[idx][0];
            best_s = sid; best_t = j.release_time; best_u = u;
        }

        timelines[best_s].schedule(best_t, j.duration, best_u,
                                    j.cpu_cores, j.memory);
        result[j.job_id] = {j.job_id, best_s, best_t, best_u, best_t + j.duration};
    }

    // Convert to 0-indexed result
    vector<ScheduleRecord> out;
    out.reserve(N);
    for (int jid = 1; jid <= N; jid++) out.push_back(result[jid]);
    return out;
}

// ============================================================
// Post-optimization: cross-server backfill + per-server compaction
// ============================================================
// Uses ServerTimeline for earliest-start queries (ported from team25).
// Significantly improves medium-case results where the event-driven
// GreedyScheduler alone underperforms vs list-scheduling.

vector<ScheduleRecord> postOptimize(
    const vector<ServerSpec> &servers,
    const vector<Job> &jobs,
    const vector<ScheduleRecord> &initial
) {
    int M = (int)servers.size();
    int N = (int)jobs.size();

    unordered_map<int, const Job*> job_map;
    for (const auto &j : jobs) job_map[j.job_id] = &j;

    unordered_map<int, const ServerSpec*> srv_map;
    for (const auto &s : servers) srv_map[s.server_id] = &s;

    // Build ServerTimelines from initial schedule (1-indexed server IDs)
    vector<ServerTimeline> timelines(M + 1);
    for (const auto &s : servers) {
        timelines[s.server_id].init(s.gpu_count, s.gpu_memory,
                                     s.cpu_cores, s.memory);
    }

    unordered_map<int, ScheduleRecord> cur;
    for (const auto &rec : initial) {
        cur[rec.job_id] = rec;
        const Job &j = *job_map[rec.job_id];
        timelines[rec.server_id].schedule(rec.start_time, j.duration,
                                           rec.gpu_used, j.cpu_cores, j.memory);
    }

    // Phase 1: Cross-server backfill (2 rounds for N<=2000, 1 for larger)
    int rounds = (N <= 2000) ? 2 : 1;
    for (int rnd = 0; rnd < rounds; rnd++) {
        vector<int> order;
        for (int jid = 1; jid <= N; jid++) order.push_back(jid);
        sort(order.begin(), order.end(), [&](int a, int b) {
            return cur[a].start_time > cur[b].start_time;
        });

        for (int jid : order) {
            const Job &j = *job_map[jid];
            ScheduleRecord &cr = cur[jid];

            // Unschedule
            timelines[cr.server_id].unschedule(cr.start_time, j.duration,
                                                cr.gpu_used, j.cpu_cores, j.memory);

            double best_cost = (double)j.weight * (cr.start_time - j.release_time)
                + (double)((long long)cr.gpu_used * srv_map[cr.server_id]->gpu_memory
                           - j.gpu_memory) * 5.0
                + (double)(cr.start_time + j.duration) * 2.0;
            int best_s = cr.server_id;
            long long best_t = cr.start_time;
            int best_u = cr.gpu_used;

            // Try all feasible servers
            for (const auto &s : servers) {
                if (j.cpu_cores > s.cpu_cores || j.memory > s.memory) continue;
                int u_mem = (j.gpu_memory + s.gpu_memory - 1) / s.gpu_memory;
                int u_min = max(j.min_gpu, u_mem);
                if (u_min > s.gpu_count) continue;

                for (int u = u_min; u <= min(u_min + 1, s.gpu_count); u++) {
                    if ((long long)u * s.gpu_memory < j.gpu_memory) continue;
                    long long t = timelines[s.server_id].earliestStart(
                        j.release_time, j.duration, u,
                        j.cpu_cores, j.memory, 200);
                    if (t < 0) continue;

                    double cost = (double)j.weight * (t - j.release_time)
                        + (double)((long long)u * s.gpu_memory - j.gpu_memory) * 5.0
                        + (double)(t + j.duration) * 2.0;
                    if (cost < best_cost - 1e-12) {
                        best_cost = cost; best_s = s.server_id;
                        best_t = t; best_u = u;
                    }
                }
            }

            timelines[best_s].schedule(best_t, j.duration, best_u,
                                        j.cpu_cores, j.memory);
            cr = {jid, best_s, best_t, best_u, best_t + j.duration};
        }
    }

    // Phase 2: Per-server compaction
    for (const auto &s : servers) {
        int sid = s.server_id;
        ServerTimeline &tl = timelines[sid];

        vector<int> sj;
        for (int jid = 1; jid <= N; jid++)
            if (cur[jid].server_id == sid) sj.push_back(jid);
        if (sj.empty()) continue;

        sort(sj.begin(), sj.end(), [&](int a, int b) {
            return cur[a].start_time < cur[b].start_time;
        });

        tl.init(s.gpu_count, s.gpu_memory, s.cpu_cores, s.memory);

        for (int jid : sj) {
            const Job &j = *job_map[jid];
            int u = cur[jid].gpu_used;
            if (u < 1) {
                int um = (j.gpu_memory + s.gpu_memory - 1) / s.gpu_memory;
                u = max(j.min_gpu, um);
                if (u < 1) u = 1;
            }

            long long t = tl.earliestStart(j.release_time, j.duration,
                                            u, j.cpu_cores, j.memory, 200);
            if (t < 0) {
                for (t = j.release_time; t <= j.release_time + 200000; t++)
                    if (tl.canStart(t, j.duration, u, j.cpu_cores, j.memory))
                        break;
            }
            tl.schedule(t, j.duration, u, j.cpu_cores, j.memory);
            cur[jid] = {jid, sid, t, u, t + j.duration};
        }
    }

    vector<ScheduleRecord> result;
    result.reserve(N);
    for (int jid = 1; jid <= N; jid++) result.push_back(cur[jid]);
    return result;
}
