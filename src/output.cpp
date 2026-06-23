#include "output.h"

#include <algorithm>

using namespace std;

static bool recordByJobId(const ScheduleRecord &a, const ScheduleRecord &b) {
    return a.job_id < b.job_id;
}

void writeScheduleRecords(ostream &output, const vector<ScheduleRecord> &records) {
    vector<ScheduleRecord> ordered = records;
    sort(ordered.begin(), ordered.end(), recordByJobId);

    for (const auto &r : ordered) {
        output << r.job_id << ' '
               << r.server_id << ' '
               << r.start_time << ' '
               << r.gpu_used << ' '
               << r.finish_time << '\n';
    }
}
