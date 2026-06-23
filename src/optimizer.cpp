#include "optimizer.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <unordered_map>
#include <iostream>

#include "machine_state.h"
#include "scheduler.h"
#include "scorer.h"
#include "strategy.h"

using namespace std;

// ============================================================
// SAOptimizer
// ============================================================

SAOptimizer::SAOptimizer(
    const vector<ServerSpec> &svrs,
    const vector<Job> &jbs,
    const Config &c
) : servers(svrs), jobs(jbs), cfg(c) {}

long long SAOptimizer::elapsedMs() const {
    auto now = chrono::steady_clock::now();
    return chrono::duration_cast<chrono::milliseconds>(now - start_time).count();
}

double SAOptimizer::energy(const EvalMetrics &m) const {
    // Composite score: normalized combination
    // Weights tuned for the three objectives
    return m.wait_score * 0.01 + m.memory_score + m.finish_score * 0.1;
}

vector<ScheduleRecord> SAOptimizer::scheduleFromOrdering(
    const vector<int> &job_order
) {
    // Build job lookup
    unordered_map<int, Job> job_map;
    for (const auto &j : jobs) job_map[j.job_id] = j;

    // Sort servers by ID
    vector<ServerSpec> sorted_servers = servers;
    sort(sorted_servers.begin(), sorted_servers.end(),
         [](const ServerSpec &a, const ServerSpec &b) { return a.server_id < b.server_id; });

    vector<MachineState> machines;
    for (const auto &s : sorted_servers) machines.emplace_back(s);
    unordered_map<int, int> midx;
    for (int i = 0; i < static_cast<int>(machines.size()); ++i) {
        midx[machines[i].spec.server_id] = i;
    }

    // Pre-compute feasible machines
    unordered_map<int, vector<pair<int, int>>> feasible;
    for (const auto &job : jobs) {
        vector<pair<int, int>> entries;
        for (int i = 0; i < static_cast<int>(machines.size()); ++i) {
            int gpu = machines[i].requiredGpuCount(job);
            if (machines[i].canEverRun(job)) {
                entries.push_back({i, gpu});
            }
        }
        feasible[job.job_id] = entries;
    }

    // Build job order index: lower = higher priority
    unordered_map<int, int> rank;
    for (int idx = 0; idx < static_cast<int>(job_order.size()); ++idx) {
        rank[job_order[idx]] = idx;
    }

    // Sort jobs by release_time for efficient time-advancing,
    // but use rank as tiebreaker within same release_time
    vector<Job> sorted_jobs = jobs;
    sort(sorted_jobs.begin(), sorted_jobs.end(),
         [&rank](const Job &a, const Job &b) {
             if (a.release_time != b.release_time) return a.release_time < b.release_time;
             return rank.at(a.job_id) < rank.at(b.job_id);
         });

    // Time-advancing greedy schedule
    long long cur_time = sorted_jobs[0].release_time;
    int next_idx = 0;
    unordered_map<int, ScheduleRecord> records;
    priority_queue<FinishEvent, vector<FinishEvent>, greater<FinishEvent>> running;
    vector<Job> pending;  // persists across iterations

    while (static_cast<int>(records.size()) < static_cast<int>(jobs.size())) {
        // Release finished
        while (!running.empty() && running.top().finish_time <= cur_time) {
            auto ev = running.top(); running.pop();
            machines[midx.at(ev.server_id)].releaseJob(ev.running_job);
        }

        // Collect newly released jobs
        while (next_idx < static_cast<int>(sorted_jobs.size()) &&
               sorted_jobs[next_idx].release_time <= cur_time) {
            pending.push_back(sorted_jobs[next_idx]);
            ++next_idx;
        }

        // Sort pending by rank (higher priority first)
        sort(pending.begin(), pending.end(),
             [&rank](const Job &a, const Job &b) {
                 return rank.at(a.job_id) < rank.at(b.job_id);
             });

        // Try to schedule (multi-pass)
        bool changed = true;
        while (changed && !pending.empty()) {
            changed = false;
            vector<Job> still;
            for (const auto &job : pending) {
                bool started = false;
                const auto &entries = feasible[job.job_id];
                for (const auto &[mi, gpu] : entries) {
                    if (machines[mi].canStart(job, gpu)) {
                        auto [rec, rj] = machines[mi].startJob(job, cur_time, gpu);
                        records[job.job_id] = rec;
                        running.push({rj.finish_time, rj.server_id, rj.job_id, rj});
                        started = true;
                        changed = true;
                        break;
                    }
                }
                if (!started) still.push_back(job);
            }
            pending = std::move(still);
        }

        if (static_cast<int>(records.size()) == static_cast<int>(jobs.size())) break;

        // Advance time
        long long next_t = INF_TIME;
        if (next_idx < static_cast<int>(sorted_jobs.size())) {
            next_t = min(next_t, static_cast<long long>(sorted_jobs[next_idx].release_time));
        }
        if (!running.empty()) {
            next_t = min(next_t, running.top().finish_time);
        }
        if (next_t == INF_TIME || next_t <= cur_time) {
            break;  // deadlock: return partial schedule (will be handled by caller)
        }
        cur_time = next_t;
    }

    // Build output
    vector<ScheduleRecord> result;
    result.reserve(records.size());
    for (int jid = 1; jid <= static_cast<int>(jobs.size()); ++jid) {
        auto it = records.find(jid);
        if (it != records.end()) result.push_back(it->second);
    }
    return result;
}

void SAOptimizer::perturb(vector<int> &order) {
    // Smart perturbation: swap two jobs with similar release times
    // This maximizes the chance that both jobs are pending simultaneously,
    // making the ordering change actually affect the schedule.
    static mt19937 rng(random_device{}());
    int n = static_cast<int>(order.size());
    uniform_int_distribution<int> idx_dist(0, n - 1);

    // Build release_time lookup
    static unordered_map<int, int> release_cache;  // job_id -> release_time
    if (release_cache.empty()) {
        for (const auto &j : jobs) release_cache[j.job_id] = j.release_time;
    }

    int i = idx_dist(rng);
    int ri = release_cache[order[i]];

    // Find candidates with release_time within ±500 of job i's release_time
    vector<int> candidates;
    int window = 500;
    for (int k = max(0, i - 50); k < min(n, i + 50); ++k) {
        if (k == i) continue;
        int rk = release_cache[order[k]];
        if (abs(rk - ri) <= window) {
            candidates.push_back(k);
        }
    }

    if (candidates.empty()) {
        // Fallback: swap with any random position
        int j = idx_dist(rng);
        if (i != j) swap(order[i], order[j]);
    } else {
        uniform_int_distribution<int> cand_dist(0, candidates.size() - 1);
        int j = candidates[cand_dist(rng)];
        swap(order[i], order[j]);
    }
}

vector<ScheduleRecord> SAOptimizer::optimize(
    const vector<ScheduleRecord> &initial_schedule
) {
    start_time = chrono::steady_clock::now();

    // Build initial job ordering from the initial schedule
    // Sort by start_time to get the effective ordering
    unordered_map<int, const ScheduleRecord*> rec_map;
    for (const auto &r : initial_schedule) {
        rec_map[r.job_id] = &r;
    }

    vector<int> best_order;
    vector<int> current_order;
    best_order.reserve(jobs.size());
    current_order.reserve(jobs.size());

    // Generate an initial ordering based on priority density
    // (start_time, weight/duration, job_id)
    vector<pair<long long, int>> start_times;
    for (const auto &job : jobs) {
        auto it = rec_map.find(job.job_id);
        long long st = (it != rec_map.end()) ? it->second->start_time : job.release_time;
        start_times.push_back({st, job.job_id});
    }
    // Sort: start_time first, then weight/duration desc for ties
    unordered_map<int, Job> job_map;
    for (const auto &j : jobs) job_map[j.job_id] = j;

    sort(start_times.begin(), start_times.end(),
         [&](const pair<long long, int> &a, const pair<long long, int> &b) {
             if (a.first != b.first) return a.first < b.first;
             const Job &ja = job_map[a.second];
             const Job &jb = job_map[b.second];
             double da = static_cast<double>(ja.weight) / ja.duration;
             double db = static_cast<double>(jb.weight) / jb.duration;
             if (da != db) return da > db;
             return a.second < b.second;
         });

    for (const auto &p : start_times) {
        current_order.push_back(p.second);
    }
    best_order = current_order;

    // Compute initial energy
    auto current_schedule = scheduleFromOrdering(current_order);
    if (current_schedule.empty()) return initial_schedule;

    auto current_metrics = computeMetrics(servers, jobs, current_schedule);
    double current_energy = energy(current_metrics);

    auto best_schedule = current_schedule;
    double best_energy = current_energy;

    // SA loop
    double temp = cfg.initial_temp;
    int total_moves = 0;
    int accepted = 0;
    int no_improve = 0;
    static mt19937 rng(random_device{}());
    uniform_real_distribution<double> uni(0.0, 1.0);

    while (temp > cfg.final_temp) {
        if (elapsedMs() > cfg.time_budget_ms) break;
        if (no_improve > cfg.no_improve_limit) break;

        for (int iter = 0; iter < cfg.iterations_per_temp; ++iter) {
            if (elapsedMs() > cfg.time_budget_ms) break;

            auto neighbor_order = current_order;
            perturb(neighbor_order);

            auto neighbor_schedule = scheduleFromOrdering(neighbor_order);
            if (neighbor_schedule.empty()) continue; // infeasible ordering, skip

            auto neighbor_metrics = computeMetrics(servers, jobs, neighbor_schedule);
            double neighbor_energy = energy(neighbor_metrics);

            double delta = neighbor_energy - current_energy;
            ++total_moves;

            if (delta < 0 || uni(rng) < exp(-delta / temp)) {
                current_order = std::move(neighbor_order);
                current_schedule = std::move(neighbor_schedule);
                current_energy = neighbor_energy;
                ++accepted;

                if (current_energy < best_energy) {
                    best_energy = current_energy;
                    best_schedule = current_schedule;
                    best_order = current_order;
                    no_improve = 0;
                }
            } else {
                ++no_improve;
            }

            if (no_improve > cfg.no_improve_limit) break;
        }

        temp *= cfg.cooling_rate;
    }

    return best_schedule;
}

// ============================================================
// LAHCOptimizer
// ============================================================

LAHCOptimizer::LAHCOptimizer(
    const vector<ServerSpec> &svrs,
    const vector<Job> &jbs,
    const Config &c
) : servers(svrs), jobs(jbs), cfg(c) {}

long long LAHCOptimizer::elapsedMs() const {
    auto now = chrono::steady_clock::now();
    return chrono::duration_cast<chrono::milliseconds>(now - start_time).count();
}

double LAHCOptimizer::energy(const EvalMetrics &m) const {
    return m.wait_score * 0.01 + m.memory_score + m.finish_score * 0.1;
}

// Reuse the same scheduleFromOrdering and perturb as SAOptimizer
// (duplicated for simplicity - same logic)

vector<ScheduleRecord> LAHCOptimizer::scheduleFromOrdering(
    const vector<int> &job_order
) {
    unordered_map<int, Job> job_map;
    for (const auto &j : jobs) job_map[j.job_id] = j;

    vector<ServerSpec> sorted_servers = servers;
    sort(sorted_servers.begin(), sorted_servers.end(),
         [](const ServerSpec &a, const ServerSpec &b) { return a.server_id < b.server_id; });

    vector<MachineState> machines;
    for (const auto &s : sorted_servers) machines.emplace_back(s);
    unordered_map<int, int> midx;
    for (int i = 0; i < static_cast<int>(machines.size()); ++i)
        midx[machines[i].spec.server_id] = i;

    unordered_map<int, vector<pair<int, int>>> feasible;
    for (const auto &job : jobs) {
        vector<pair<int, int>> entries;
        for (int i = 0; i < static_cast<int>(machines.size()); ++i) {
            int gpu = machines[i].requiredGpuCount(job);
            if (machines[i].canEverRun(job)) entries.push_back({i, gpu});
        }
        feasible[job.job_id] = entries;
    }

    unordered_map<int, int> rank;
    for (int idx = 0; idx < static_cast<int>(job_order.size()); ++idx)
        rank[job_order[idx]] = idx;

    vector<Job> sorted_jobs = jobs;
    sort(sorted_jobs.begin(), sorted_jobs.end(),
         [&rank](const Job &a, const Job &b) {
             if (a.release_time != b.release_time) return a.release_time < b.release_time;
             return rank.at(a.job_id) < rank.at(b.job_id);
         });

    long long cur_time = sorted_jobs[0].release_time;
    int next_idx = 0;
    unordered_map<int, ScheduleRecord> records;
    priority_queue<FinishEvent, vector<FinishEvent>, greater<FinishEvent>> running;
    vector<Job> pending;

    while (static_cast<int>(records.size()) < static_cast<int>(jobs.size())) {
        while (!running.empty() && running.top().finish_time <= cur_time) {
            auto ev = running.top(); running.pop();
            machines[midx.at(ev.server_id)].releaseJob(ev.running_job);
        }
        while (next_idx < static_cast<int>(sorted_jobs.size()) &&
               sorted_jobs[next_idx].release_time <= cur_time) {
            pending.push_back(sorted_jobs[next_idx]); ++next_idx;
        }
        sort(pending.begin(), pending.end(),
             [&rank](const Job &a, const Job &b) {
                 return rank.at(a.job_id) < rank.at(b.job_id);
             });

        bool changed = true;
        while (changed && !pending.empty()) {
            changed = false;
            vector<Job> still;
            for (const auto &job : pending) {
                bool started = false;
                for (const auto &[mi, gpu] : feasible[job.job_id]) {
                    if (machines[mi].canStart(job, gpu)) {
                        auto [rec, rj] = machines[mi].startJob(job, cur_time, gpu);
                        records[job.job_id] = rec;
                        running.push({rj.finish_time, rj.server_id, rj.job_id, rj});
                        started = true; changed = true; break;
                    }
                }
                if (!started) still.push_back(job);
            }
            pending = std::move(still);
        }

        if (static_cast<int>(records.size()) == static_cast<int>(jobs.size())) break;

        long long next_t = INF_TIME;
        if (next_idx < static_cast<int>(sorted_jobs.size()))
            next_t = min(next_t, static_cast<long long>(sorted_jobs[next_idx].release_time));
        if (!running.empty()) next_t = min(next_t, running.top().finish_time);
        if (next_t == INF_TIME || next_t <= cur_time) break;
        cur_time = next_t;
    }

    vector<ScheduleRecord> result;
    result.reserve(records.size());
    for (int jid = 1; jid <= static_cast<int>(jobs.size()); ++jid) {
        auto it = records.find(jid);
        if (it != records.end()) result.push_back(it->second);
    }
    return result;
}

void LAHCOptimizer::perturb(vector<int> &order) {
    static mt19937 rng(random_device{}());
    int n = static_cast<int>(order.size());
    uniform_int_distribution<int> idx_dist(0, n - 1);

    static unordered_map<int, int> release_cache;
    if (release_cache.empty())
        for (const auto &j : jobs) release_cache[j.job_id] = j.release_time;

    int i = idx_dist(rng);
    int ri = release_cache[order[i]];
    vector<int> candidates;
    for (int k = max(0, i - 50); k < min(n, i + 50); ++k) {
        if (k == i) continue;
        if (abs(release_cache[order[k]] - ri) <= 500)
            candidates.push_back(k);
    }
    if (candidates.empty()) {
        int j = idx_dist(rng);
        if (i != j) swap(order[i], order[j]);
    } else {
        uniform_int_distribution<int> cand_dist(0, candidates.size() - 1);
        swap(order[i], order[candidates[cand_dist(rng)]]);
    }
}

vector<ScheduleRecord> LAHCOptimizer::optimize(
    const vector<ScheduleRecord> &initial_schedule
) {
    start_time = chrono::steady_clock::now();

    // Build initial ordering from initial schedule
    unordered_map<int, const ScheduleRecord*> rec_map;
    for (const auto &r : initial_schedule) rec_map[r.job_id] = &r;

    unordered_map<int, Job> job_map;
    for (const auto &j : jobs) job_map[j.job_id] = j;

    vector<pair<long long, int>> start_times;
    for (const auto &job : jobs) {
        auto it = rec_map.find(job.job_id);
        long long st = (it != rec_map.end()) ? it->second->start_time : job.release_time;
        start_times.push_back({st, job.job_id});
    }
    sort(start_times.begin(), start_times.end(),
         [&](const pair<long long, int> &a, const pair<long long, int> &b) {
             if (a.first != b.first) return a.first < b.first;
             const Job &ja = job_map[a.second], &jb = job_map[b.second];
             double da = static_cast<double>(ja.weight) / ja.duration;
             double db = static_cast<double>(jb.weight) / jb.duration;
             if (da != db) return da > db;
             return a.second < b.second;
         });

    vector<int> current_order, best_order;
    for (const auto &p : start_times) current_order.push_back(p.second);
    best_order = current_order;

    auto current_schedule = scheduleFromOrdering(current_order);
    if (current_schedule.empty()) return initial_schedule;

    auto current_metrics = computeMetrics(servers, jobs, current_schedule);
    double current_energy = energy(current_metrics);
    auto best_schedule = current_schedule;
    double best_energy = current_energy;

    // LAHC: maintain history of past energies
    vector<double> history(cfg.history_length, current_energy);
    int iter = 0;
    int no_improve = 0;

    while (true) {
        if (elapsedMs() > cfg.time_budget_ms) break;
        if (no_improve > cfg.no_improve_limit) break;

        auto neighbor_order = current_order;
        perturb(neighbor_order);
        auto neighbor_schedule = scheduleFromOrdering(neighbor_order);
        if (neighbor_schedule.empty()) continue;

        auto neighbor_metrics = computeMetrics(servers, jobs, neighbor_schedule);
        double neighbor_energy = energy(neighbor_metrics);

        // Accept if better than the value L iterations ago
        int idx = iter % cfg.history_length;
        double old_energy = history[idx];

        if (neighbor_energy < old_energy) {
            current_order = std::move(neighbor_order);
            current_schedule = std::move(neighbor_schedule);
            current_energy = neighbor_energy;
            no_improve = 0;

            if (current_energy < best_energy) {
                best_energy = current_energy;
                best_schedule = current_schedule;
                best_order = current_order;
            }
        } else {
            ++no_improve;
        }

        // Update history
        history[idx] = current_energy;
        ++iter;
    }

    return best_schedule;
}
