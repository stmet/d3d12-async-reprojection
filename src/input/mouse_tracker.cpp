#include "mouse_tracker.h"
#include <atomic>

namespace MouseTracker {
namespace {

std::atomic<long long> s_accX{0};
std::atomic<long long> s_accY{0};

struct Sample { uint64_t qpc; long long ax, ay; };
constexpr int kRing = 1024;
Sample s_ring[kRing] = {};
std::atomic<uint32_t> s_head{0};   // next write index, published with release

int64_t s_qpcFreq = 0;
int64_t QpcFreq() {
    if (s_qpcFreq == 0) { LARGE_INTEGER li; QueryPerformanceFrequency(&li); s_qpcFreq = li.QuadPart; }
    return s_qpcFreq;
}

} // namespace

uint64_t NowQpc() {
    LARGE_INTEGER li; QueryPerformanceCounter(&li); return (uint64_t)li.QuadPart;
}

int64_t MsToQpc(double ms) {
    return (int64_t)(ms * (double)QpcFreq() / 1000.0);
}

void OnRawInput(LPARAM lParam) {
    UINT size = 0;
    GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
    if (size == 0 || size > 128) return;
    BYTE buf[128];
    if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) != size) return;

    RAWINPUT* ri = (RAWINPUT*)buf;
    if (ri->header.dwType != RIM_TYPEMOUSE) return;
    if (ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) return; // ignore absolute (tablets/RDP)

    long long nx = s_accX.fetch_add(ri->data.mouse.lLastX, std::memory_order_relaxed) + ri->data.mouse.lLastX;
    long long ny = s_accY.fetch_add(ri->data.mouse.lLastY, std::memory_order_relaxed) + ri->data.mouse.lLastY;

    uint32_t i = s_head.load(std::memory_order_relaxed);
    s_ring[i % kRing] = { NowQpc(), nx, ny };
    s_head.store(i + 1, std::memory_order_release);
}

void GetAccAt(uint64_t targetQpc, long long& ax, long long& ay) {
    uint32_t head = s_head.load(std::memory_order_acquire);
    int count = (head < (uint32_t)kRing) ? (int)head : kRing;
    if (count == 0) { ax = s_accX.load(std::memory_order_relaxed); ay = s_accY.load(std::memory_order_relaxed); return; }

    const Sample& newest = s_ring[(head - 1) % kRing];

    if (targetQpc > newest.qpc) {
        // FUTURE of the newest sample (the present-time latch). Raw-input samples only arrive when the
        // game pumps its message queue (~base fps), so at a low cap (e.g. 30 fps) there is no fresh
        // sample for most of the gap and consecutive presents would reuse the same position -> duplicate
        // warped frames + stepped motion. Extrapolate forward from the recent velocity so every present
        // advances smoothly. Constant-velocity assumption (exact for smooth pans, slight chordal error
        // on direction changes); the horizon is clamped so a sudden stop only briefly overshoots.
        const Sample* older = &newest;
        int64_t velWindow = MsToQpc(12.0);
        for (int k = 2; k <= count; ++k) {
            const Sample& s = s_ring[(head - k) % kRing];
            older = &s;
            if ((int64_t)(newest.qpc - s.qpc) >= velWindow) break;
        }
        int64_t dt = (int64_t)(newest.qpc - older->qpc);
        if (dt > 0) {
            int64_t horizon = (int64_t)(targetQpc - newest.qpc);
            int64_t maxHorizon = MsToQpc(40.0);
            if (horizon > maxHorizon) horizon = maxHorizon;
            double vx = (double)(newest.ax - older->ax) / (double)dt;
            double vy = (double)(newest.ay - older->ay) / (double)dt;
            ax = newest.ax + (long long)(vx * (double)horizon);
            ay = newest.ay + (long long)(vy * (double)horizon);
        } else {
            ax = newest.ax; ay = newest.ay;
        }
        return;
    }

    // PAST time (the warp base): return the actual historical accumulated value, no extrapolation.
    for (int k = 1; k <= count; ++k) {
        const Sample& s = s_ring[(head - k) % kRing];
        if (s.qpc <= targetQpc) { ax = s.ax; ay = s.ay; return; }
    }
    const Sample& oldest = s_ring[(head - count) % kRing];
    ax = oldest.ax; ay = oldest.ay;
}

} // namespace MouseTracker
