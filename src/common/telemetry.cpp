#include "telemetry.h"
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <mutex>

namespace Telemetry {
namespace {

HANDLE        s_file = INVALID_HANDLE_VALUE;
std::mutex    s_mtx;
LARGE_INTEGER s_freq = {};
LARGE_INTEGER s_start = {};

double NowMs() {
    LARGE_INTEGER n; QueryPerformanceCounter(&n);
    return s_freq.QuadPart ? (double)(n.QuadPart - s_start.QuadPart) * 1000.0 / (double)s_freq.QuadPart : 0.0;
}

void WriteRaw(const char* buf, int len) {
    if (s_file == INVALID_HANDLE_VALUE || len <= 0) return;
    DWORD wrote = 0;
    WriteFile(s_file, buf, (DWORD)len, &wrote, nullptr);
}

// Shared column layout for STAT and EVENT rows. `event` is empty for STAT; the metric columns are
// empty for EVENT (the config snapshot is still written so every event carries its full context).
void WriteRow(const char* kind, const char* event, const Sample& s, bool withMetrics) {
    char buf[1024];
    int n = snprintf(buf, sizeof(buf),
        "%.1f,%s,%s,"
        "%d,%d,%d,%.4f,%.1f,%.5f,%+.0f,%d,%.2f,%d,%.3f,%.3f,%d,%d,%d,%d,",
        NowMs(), kind, event ? event : "",
        s.enabled, s.mode, s.angular, s.sens, s.fov, s.gain, s.sign,
        s.autoTrim, s.trimMs, s.autoLead, s.leadMs, s.leadFloorMs, s.maxFif, s.vsync, s.lateWarp, s.asyncCompute);
    if (withMetrics) {
        n += snprintf(buf + n, sizeof(buf) - n,
            "%.1f,%.1f,%.1f,%.3f,%.3f,%.3f,%llu,%.2f,%.3f,%d\n",
            s.presentFps, s.gameFps, s.refreshHz, s.inputAgeMs, s.gameAgeMs, s.jitterMs,
            (unsigned long long)s.missedVblanks, s.gpuDepth, s.warpMs, s.compute);
    } else {
        n += snprintf(buf + n, sizeof(buf) - n, ",,,,,,,,,\n");  // 10 empty metric columns
    }
    WriteRaw(buf, n);
}

} // namespace

void Init(HMODULE dllModule) {
    std::lock_guard<std::mutex> lk(s_mtx);
    if (s_file != INVALID_HANDLE_VALUE) return;

    QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&s_start);

    wchar_t path[MAX_PATH];
    std::wstring out = L"dxgi_telemetry.csv";
    if (GetModuleFileNameW(dllModule, path, MAX_PATH) != 0) {
        std::wstring p(path);
        size_t slash = p.find_last_of(L"\\/");
        if (slash != std::wstring::npos) out = p.substr(0, slash + 1) + L"dxgi_telemetry.csv";
    }
    s_file = CreateFileW(out.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (s_file == INVALID_HANDLE_VALUE) return;

    const char* header =
        "t_ms,kind,event,"
        "enabled,mode,angular,sens,fov,gain,sign,autotrim,trim_ms,autolead,lead_ms,lead_floor_ms,maxfif,vsync,latewarp,async_compute,"
        "present_fps,game_fps,refresh_hz,input_age_ms,game_age_ms,jitter_ms,missed_vblanks,gpu_depth,warp_ms,compute\n";
    WriteRaw(header, (int)strlen(header));
}

void Stat(const Sample& s) {
    std::lock_guard<std::mutex> lk(s_mtx);
    WriteRow("STAT", "", s, true);
}

void Event(const Sample& s, const char* fmt, ...) {
    char msg[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(s_mtx);
    WriteRow("EVENT", msg, s, false);
}

void Shutdown() {
    std::lock_guard<std::mutex> lk(s_mtx);
    if (s_file != INVALID_HANDLE_VALUE) { CloseHandle(s_file); s_file = INVALID_HANDLE_VALUE; }
}

} // namespace Telemetry
