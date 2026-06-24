#ifndef GPU_SCHEDULING_TIMELINE_H
#define GPU_SCHEDULING_TIMELINE_H

#include <utility>
#include <vector>
#include "models.h"

// Per-server resource timeline via delta events.
class ServerTimeline {
public:
    void init(int g, int vg, int c, int r);
    bool canStart(long long t, int dur, int gpu, int cpu, int ram) const;
    long long earliestStart(long long r, int dur, int gpu,
                            int cpu, int ram, int max_iter) const;
    void schedule(long long t, int dur, int gpu, int cpu, int ram);
    void unschedule(long long t, int dur, int gpu, int cpu, int ram);
    int totalGpu() const { return G; }
    int totalGpuMemory() const { return G * VG; }
    int totalCpu() const { return C; }
    int totalRam() const { return R; }

private:
    static void addDelta(std::vector<std::pair<long long, long long>>& ev,
                         long long t, long long d);
    bool resourceOk(const std::vector<std::pair<long long, long long>>& ev,
                    long long t, long long dur, long long need, long long cap) const;
    int G = 0, VG = 0, C = 0, R = 0;
    std::vector<std::pair<long long, long long>> gpu_ev, cpu_ev, ram_ev;
};

#endif
