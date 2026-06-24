#include "timeline.h"

#include <algorithm>
#include <climits>

using namespace std;

void ServerTimeline::init(int g, int vg, int c, int r) {
    G = g; VG = vg; C = c; R = r;
    gpu_ev.clear(); cpu_ev.clear(); ram_ev.clear();
}

void ServerTimeline::addDelta(vector<pair<long long, long long>>& ev,
                               long long t, long long d) {
    int lo = 0, hi = (int)ev.size();
    while (lo < hi) { int m = (lo + hi) / 2; ev[m].first < t ? lo = m + 1 : hi = m; }
    if (lo < (int)ev.size() && ev[lo].first == t) ev[lo].second += d;
    else ev.insert(ev.begin() + lo, {t, d});
}

bool ServerTimeline::resourceOk(const vector<pair<long long, long long>>& ev,
                                 long long t, long long dur,
                                 long long need, long long cap) const {
    long long t_end = t + dur;
    long long cur = 0;
    size_t i = 0;
    while (i < ev.size() && ev[i].first <= t) { cur += ev[i].second; i++; }
    if (cur + need > cap) return false;
    long long prev = t;
    while (i < ev.size() && ev[i].first < t_end) {
        if (ev[i].first > prev && cur + need > cap) return false;
        cur += ev[i].second; prev = ev[i].first; i++;
    }
    if (t_end > prev && cur + need > cap) return false;
    return true;
}

bool ServerTimeline::canStart(long long t, int dur,
                               int gpu, int cpu, int ram) const {
    return resourceOk(gpu_ev, t, dur, gpu, G) &&
           resourceOk(cpu_ev, t, dur, cpu, C) &&
           resourceOk(ram_ev, t, dur, ram, R);
}

long long ServerTimeline::earliestStart(long long r, int dur,
                                         int gpu, int cpu, int ram,
                                         int max_iter) const {
    long long t = r;
    for (int it = 0; it < max_iter; it++) {
        if (resourceOk(gpu_ev, t, dur, gpu, G) &&
            resourceOk(cpu_ev, t, dur, cpu, C) &&
            resourceOk(ram_ev, t, dur, ram, R))
            return t;

        long long nx = LLONG_MAX;
        auto nextTime = [&](const vector<pair<long long, long long>>& ev) {
            int lo = 0, hi = (int)ev.size();
            while (lo < hi) { int m = (lo + hi) / 2; ev[m].first <= t ? lo = m + 1 : hi = m; }
            if (lo < (int)ev.size() && ev[lo].first < nx) nx = ev[lo].first;
        };
        nextTime(gpu_ev); nextTime(cpu_ev); nextTime(ram_ev);
        t = (nx == LLONG_MAX) ? t + 1 : max(t + 1, nx);
    }
    return -1;
}

void ServerTimeline::schedule(long long t, int dur,
                               int gpu, int cpu, int ram) {
    addDelta(gpu_ev, t, gpu);     addDelta(gpu_ev, t + dur, -gpu);
    addDelta(cpu_ev, t, cpu);     addDelta(cpu_ev, t + dur, -cpu);
    addDelta(ram_ev, t, ram);     addDelta(ram_ev, t + dur, -ram);
}

void ServerTimeline::unschedule(long long t, int dur,
                                 int gpu, int cpu, int ram) {
    addDelta(gpu_ev, t, -gpu);    addDelta(gpu_ev, t + dur, gpu);
    addDelta(cpu_ev, t, -cpu);    addDelta(cpu_ev, t + dur, cpu);
    addDelta(ram_ev, t, -ram);    addDelta(ram_ev, t + dur, ram);
}
