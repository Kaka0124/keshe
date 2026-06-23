// Standalone evaluation tool: reads input + output files
// Usage: ./evaluate <case.in> <case.out>
// Prints raw metrics and normalized instance score.

#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>

#include "../src/scorer.h"
#include "../src/constraint_checker.h"
#include "../src/parser.h"

using namespace std;

static string readFile(const string &path) {
    ifstream f(path);
    if (!f) {
        cerr << "ERROR: cannot open " << path << endl;
        exit(1);
    }
    ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        cerr << "Usage: ./evaluate <case.in> <case.out>" << endl;
        return 1;
    }

    string in_text  = readFile(argv[1]);
    string out_text = readFile(argv[2]);

    // Parse input
    auto [servers, jobs] = parseInstance(in_text);
    int M = static_cast<int>(servers.size());
    int N = static_cast<int>(jobs.size());

    // Parse output
    vector<ScheduleRecord> records;
    istringstream out_stream(out_text);
    string line;
    while (getline(out_stream, line)) {
        if (line.empty()) continue;
        istringstream ls(line);
        ScheduleRecord rec;
        if (!(ls >> rec.job_id >> rec.server_id
                  >> rec.start_time >> rec.gpu_used >> rec.finish_time)) {
            cerr << "ERROR: malformed line: " << line << endl;
            return 1;
        }
        records.push_back(rec);
    }

    cout << "========== Evaluation ==========" << endl;
    cout << "Servers: " << M << "  Jobs: " << N << endl;
    cout << "Output records: " << records.size() << endl;

    // 1. Legality check
    auto ck = checkSchedule(servers, jobs, records);
    cout << endl << "--- Legality ---" << endl;
    cout << (ck.valid ? "PASS" : "FAIL: " + ck.error_msg) << endl;

    if (!ck.valid) {
        cout << endl << "Instance score E = 3.0 (illegal)" << endl;
        return 0;
    }

    // 2. Raw metrics (spec §二-§四)
    auto m = computeMetrics(servers, jobs, records);

    cout << endl << "--- Raw Metrics ---" << endl;
    cout << fixed << setprecision(2);
    cout << "E_wait   = " << m.wait_score << endl;
    cout << "  (= sum w_i * (t_i - r_i), weighted wait time)" << endl;
    cout << "E_memory = " << m.memory_score << " GB" << endl;
    cout << "  (= avg GPU memory idle per time slot across all servers)" << endl;
    cout << "E_finish = " << m.finish_score << endl;
    cout << "  (= H = max finish time, aka makespan)" << endl;

    // 3. Normalized score (spec §五)
    // Without other submissions, use our own values as both best and worst
    // to show what the formula produces.
    InstanceBestWorst bw;
    bw.best_wait   = 0.0;           // ideal: zero wait
    bw.worst_wait  = m.wait_score;  // our raw value
    bw.best_memory = 0.0;           // ideal: zero idle
    bw.worst_memory = m.memory_score;
    bw.best_finish = 0;             // ideal: instant finish
    bw.worst_finish = m.finish_score;

    double score = computeInstanceScore(m, bw);
    cout << endl << "--- Normalized (self-referenced) ---" << endl;
    cout << "E_jk = " << setprecision(6) << score << endl;
    cout << "  (each term vs ideal=0, worst=self)" << endl;

    // Breakdown
    double wt = (bw.worst_wait > bw.best_wait)
        ? (m.wait_score - bw.best_wait) / (bw.worst_wait - bw.best_wait) : 0.0;
    double mt = (bw.worst_memory > bw.best_memory)
        ? (m.memory_score - bw.best_memory) / (bw.worst_memory - bw.best_memory) : 0.0;
    double ft = (bw.worst_finish > bw.best_finish)
        ? static_cast<double>(m.finish_score - bw.best_finish)
            / static_cast<double>(bw.worst_finish - bw.best_finish) : 0.0;
    cout << "  wait_term=" << wt << " mem_term=" << mt << " finish_term=" << ft << endl;

    // 4. Note about real evaluation
    cout << endl << "--- Note ---" << endl;
    cout << "Real evaluation uses Best/Worst from ALL submissions." << endl;
    cout << "Self-referenced score shown above is for debugging only." << endl;

    return 0;
}
