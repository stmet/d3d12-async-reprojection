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
    ax = s_accX.load(std::memory_order_relaxed);
    ay = s_accY.load(std::memory_order_relaxed);
    int count = (head < (uint32_t)kRing) ? (int)head : kRing;
    for (int k = 1; k <= count; ++k) {
        const Sample& s = s_ring[(head - k) % kRing];
        if (s.qpc <= targetQpc) { ax = s.ax; ay = s.ay; return; }
    }
    if (count > 0) {
        const Sample& s = s_ring[(head - count) % kRing];
        ax = s.ax; ay = s.ay;
    }
}

} // namespace MouseTracker
