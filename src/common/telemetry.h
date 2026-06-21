#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Structured telemetry sink (CSV) for the lean reprojection presenter. Separate from the human-
// readable dxgi.log: this writes one machine-parseable row per ~1 Hz stat window (STAT) plus a row
// whenever a tunable changes (EVENT), so we can correlate "I enabled late-warp" with "input->scanout
// dropped" offline (analyze_telemetry.ps1) instead of eyeballing the overlay.
//
// Honesty note: input->scanout is anchored to the vblank ONLY when presents are phase-locked to the
// refresh. That is why every STAT row also carries jitter + missed-vblank deltas — the analyzer gates
// the latency claim on present stability so we never trust a hallucinated number.
namespace Telemetry {

// One snapshot: the live config + the windowed metrics. The presenter fills it; STAT rows use every
// field, EVENT rows use the config fields (metrics left blank) so each event is self-contained.
struct Sample {
    // ---- config snapshot ----
    int   enabled      = 0;
    int   mode         = 0;
    int   angular      = 0;
    float sens         = 0.0f;   // deg / 1000 counts (angular model)
    float fov          = 0.0f;   // vertical deg
    float gain         = 0.0f;   // legacy UV gain
    float sign         = 0.0f;
    int   autoTrim     = 0;
    float trimMs       = 0.0f;
    int   autoLead     = 0;
    float leadMs       = 0.0f;
    float leadFloorMs  = 0.0f;
    int   maxFif       = 0;
    int   vsync        = 0;
    int   lateWarp     = 0;
    int   asyncCompute = 0;   // config: async-compute warp requested
    // ---- windowed metrics (STAT rows) ----
    float presentFps   = 0.0f;
    float gameFps      = 0.0f;
    float refreshHz    = 0.0f;
    float inputAgeMs   = 0.0f;
    float gameAgeMs    = 0.0f;
    float jitterMs     = 0.0f;
    unsigned long long missedVblanks = 0;  // cumulative (analyzer differences successive rows)
    float gpuDepth     = 0.0f;
    float warpMs       = 0.0f;   // measured warp GPU time (compute path timestamps)
    int   compute      = 0;      // warp ran on the async-compute queue this window
};

// Open <dll dir>/dxgi_telemetry.csv (CREATE_ALWAYS) and write the header. Safe to call once.
void Init(HMODULE dllModule);

// Append a STAT row (the ~1 Hz window) — config + metrics.
void Stat(const Sample& s);

// Append an EVENT row: a printf-style label (e.g. "warp.enable=1") plus the current config snapshot.
void Event(const Sample& s, const char* fmt, ...);

void Shutdown();

} // namespace Telemetry
