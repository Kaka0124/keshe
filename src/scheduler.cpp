#include "scheduler.h"

#include <algorithm>
#include <stdexcept>
#include <iostream>

#include "machine_state.h"
#include "scorer.h"

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
    // Try several strategy combos, pick the one with best score
    struct Combo {
        JobSortStrategy js;
        MachineSelectStrategy ms;
    };

    vector<Combo> combos = {
        {JobSortStrategy::BY_PRIORITY_DENSITY, MachineSelectStrategy::BEST_FIT_COMBINED},
        {JobSortStrategy::BY_PRIORITY_DENSITY, MachineSelectStrategy::BEST_FIT_GPU},
        {JobSortStrategy::BY_RESOURCE_ASC,    MachineSelectStrategy::BEST_FIT_COMBINED},
        {JobSortStrategy::BY_SHORTEST_FIRST,  MachineSelectStrategy::BEST_FIT_COMBINED},
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
            // Simple composite score (lower is better)
            double score = metrics.wait_score * 0.001 + metrics.memory_score + metrics.finish_score * 0.01;

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
