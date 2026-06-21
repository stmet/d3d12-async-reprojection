#include "presenter.h"
#include "../common/logger.h"
#include "../common/telemetry.h"
#include "../hooks/hook_manager.h"
#include "../overlay/overlay.h"
#include "../warp/warp_renderer.h"
#include "../input/mouse_tracker.h"
#include "../capture/depth_capture.h"

#include <thread>
#include <mutex>
#include <atomic>
#include <cmath>
#include <intrin.h>
#include <dwmapi.h>
#include <timeapi.h>
#pragma comment(lib, "dwmapi")
#pragma comment(lib, "winmm")

namespace Presenter {
namespace {

PresenterParams            s_params;

ID3D12Device*              s_device       = nullptr;
ID3D12CommandQueue*        s_presentQueue = nullptr;   // real swapchain presents on this
ID3D12CommandQueue*        s_gameQueue    = nullptr;   // the game's own queue (fenced)
IDXGISwapChain4*           s_real         = nullptr;

ID3D12Fence*               s_gameFence    = nullptr;   // signalled on s_gameQueue per game frame
UINT64                     s_gameFenceVal = 0;

// Drain fence for the presenter queue (Stop()).
ID3D12Fence*               s_drainFence   = nullptr;
UINT64                     s_drainVal     = 0;
HANDLE                     s_drainEvent   = nullptr;

// CPU event for the frame-in-flight limiter (SubmitGameFrame blocks the game thread on s_gameFence).
HANDLE                     s_limiterEvent = nullptr;

std::mutex                 s_mtx;
ID3D12Resource*            s_latestFrame  = nullptr;    // newest replacement backbuffer
UINT64                     s_latestFenceVal = 0;        // game-fence value that retires it
uint64_t                   s_latestSubmitQpc = 0;       // QPC when the game submitted that frame
bool                       s_haveFrame    = false;

// Short history of recent game submissions, so the presenter can pick the FRESHEST frame the GPU has
// actually finished — the game runs 2-3 frames ahead of GPU completion when GPU-bound, so the single
// newest submission is usually still in flight. Sized to the proxy's pool max (kMaxBuffers = 8); the
// usable search depth is bounded to (bufferCount - 1) at read time because older entries alias VRAM
// the game is already overwriting. Written under s_mtx in SubmitGameFrame.
struct FrameRec { ID3D12Resource* frame; UINT64 fenceVal; uint64_t submitQpc; };
constexpr int              kFrameHistory = 8;
FrameRec                   s_frameHist[kFrameHistory] = {};
uint64_t                   s_frameHistCount = 0;        // total submissions (head = count % kFrameHistory)
UINT                       s_bufferCount  = 2;          // replacement-pool size (cached at Start)

// Game-frame timing for the MV extrapolation factor (how far past the captured frame we are).
std::atomic<uint64_t>      s_lastGameQpc{0};
std::atomic<int64_t>       s_gameIntervalQpc{0};        // smoothed interval between game frames

// Display dimensions (cached at Start) — used to normalize the mouse delta for gain calibration,
// matching the warp's own screen-width normalization so the regressed slope IS the warp gain.
float                      s_dispW = 0.0f, s_dispH = 0.0f;
float                      s_baseFov = 0.0f;   // hip-fire FOV baseline (slow max) for FOV-based ADS detection

std::thread                s_thread;
std::atomic<bool>          s_running{false};

// Vblank pacing state (presenter thread only).
uint64_t                   s_lastVblankQpc = 0;   // QPC right after the last real present (~vblank when vsync on)
int64_t                    s_refreshQpc    = 0;   // estimated display refresh period, in QPC ticks

// Query the display refresh period (QPC ticks) from DWM. Stable across the session; used to pace when
// vsync is off (where we can't infer the period from our own immediate presents) and to seed the EMA.
int64_t QueryRefreshQpc() {
    DWM_TIMING_INFO ti = {}; ti.cbSize = sizeof(ti);
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.qpcRefreshPeriod > 0)
        return (int64_t)ti.qpcRefreshPeriod;
    return MouseTracker::MsToQpc(1000.0 / 60.0);   // 60 Hz fallback
}

// High-resolution waitable timer for the coarse sleep stage. CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
// (Win10 1803+) has far less wakeup jitter than ::Sleep's ~1 ms scheduler quantum, so we can pace
// closer to the vblank with a smaller spin window. Falls back to a normal timer / ::Sleep if unavailable.
HANDLE s_waitTimer = nullptr;

// Sleep until `targetQpc`: high-res timer for the bulk, then spin the last ~0.3 ms so we wake within
// tens of microseconds of the target (minimises both missed vblanks and excess input age).
void SleepUntilQpc(uint64_t targetQpc) {
    if (!s_waitTimer) {
        s_waitTimer = CreateWaitableTimerExW(nullptr, nullptr,
                          CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!s_waitTimer) s_waitTimer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    const int64_t spinMargin = MouseTracker::MsToQpc(0.3);   // spin the final 0.3 ms for precision
    const double  ticksPerMs = (double)MouseTracker::MsToQpc(1.0);
    for (;;) {
        uint64_t now = MouseTracker::NowQpc();
        if (now >= targetQpc) return;
        int64_t rem = (int64_t)(targetQpc - now);
        if (rem > spinMargin && s_waitTimer) {
            // Wait until (target - spinMargin). SetWaitableTimer takes 100 ns units, negative = relative.
            double remMs = (double)(rem - spinMargin) / ticksPerMs;
            LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(remMs * 10000.0);
            if (SetWaitableTimer(s_waitTimer, &due, 0, nullptr, nullptr, FALSE))
                WaitForSingleObject(s_waitTimer, 50);
            else
                ::Sleep(0);
        } else {
            _mm_pause();
        }
    }
}

// Adaptive game-thread delay (ported from low_latency_layer/delay_controller.cc). Called once per game
// frame on the GAME thread (end of SubmitGameFrame = the game's Present, i.e. its frame boundary). It
// holds the game a tuned amount so the next frame samples input later -> fresher content, without
// dropping throughput. Cyberpunk decouples sim from render, so a fixed cap can't pin the sim queue;
// instead we jitter the delay and watch whether the game-frame interval tracks the jitter (gradient
// ~1 => at the sim floor, stop adding) or not (~0 => backlogged, drain more). EWMA centers at the knee.
// State is touched only on the game thread -> no locking.
namespace dc {
    bool     have         = false;
    int64_t  prevFrametime = 0;
    int64_t  prevJitter    = 0;
    uint64_t prevRelease   = 0;
    double   gradientEwma   = 0.5;   // START at the target: no delay until evidence says it helps
    int64_t  drain          = 0;

    void Reset() { have = false; prevFrametime = 0; prevJitter = 0; prevRelease = 0; gradientEwma = 0.5; drain = 0; }

    void Delay() {
        uint64_t now = MouseTracker::NowQpc();
        if (!have) { prevRelease = now; have = true; return; }
        int64_t frametime = (int64_t)(now - prevRelease);
        if (frametime <= 0) { prevRelease = now; return; }

        // Hard safety ceiling: never hold more than half a frame, and never more than a few ms — the
        // earlier version pinned the drain at a full frame and tanked pacing. Sleep (don't spin) for
        // the hold so we never burn a core on the game thread.
        int64_t maxDrain = frametime / 2;
        const int64_t cap6ms = MouseTracker::MsToQpc(6.0);
        if (maxDrain > cap6ms) maxDrain = cap6ms;
        if (drain > maxDrain) drain = maxDrain;

        int64_t jitter = (prevJitter == 0) ? frametime / 50 : 0;
        SleepUntilQpc(now + (uint64_t)(jitter + drain));

        if (prevJitter == 0) {
            // This frame applied the jitter; the gradient is computed on the next (non-jittered) frame.
            prevFrametime = frametime; prevJitter = jitter; prevRelease = MouseTracker::NowQpc();
            return;
        }
        // gradient = how much removing the previous frame's jitter changed the frametime.
        double dtJitter    = -(double)prevJitter;
        double dtFrametime = (double)(frametime - prevFrametime);
        double gradient    = (dtJitter != 0.0) ? dtFrametime / dtJitter : 0.0;
        if (gradient >  1.0) gradient =  1.0;
        if (gradient < -1.0) gradient = -1.0;
        constexpr double ALPHA = 0.02;
        gradientEwma = ALPHA * gradient + (1.0 - ALPHA) * gradientEwma;
        // Gentle: small gain, and only nudge — clamped to [0, maxDrain]. Drains UP slowly toward the
        // sim floor, backs OFF the instant the gradient says the delay is costing real frametime.
        constexpr double DELTA_GAIN = 0.5;
        double delta = DELTA_GAIN * (0.5 - gradientEwma) * (double)prevJitter;
        drain += (int64_t)delta;
        if (drain < 0)        drain = 0;
        if (drain > maxDrain) drain = maxDrain;
        prevFrametime = frametime; prevJitter = jitter; prevRelease = MouseTracker::NowQpc();
    }
}

// ---- structured telemetry ----
// Fill the config half of a sample from the live tunables (read each window + on change detection).
Telemetry::Sample s_lastCfg;
bool              s_cfgInit = false;

void FillConfig(Telemetry::Sample& s) {
    WarpParams& wp = WarpRenderer::Params();
    s.enabled     = wp.enable ? 1 : 0;
    s.mode        = wp.mode;
    s.angular     = wp.angularGain ? 1 : 0;
    s.sens        = wp.sensDegPer1000;
    s.fov         = wp.fovDeg;
    s.gain        = wp.gain;
    s.sign        = wp.sign;
    s.autoTrim    = wp.autoTrim ? 1 : 0;
    s.trimMs      = wp.trimMs;
    s.autoLead    = s_params.autoLead ? 1 : 0;
    s.leadMs      = s_params.leadMs;
    s.leadFloorMs = s_params.leadFloorMs;
    s.maxFif      = s_params.maxFramesInFlight;
    s.vsync       = s_params.syncInterval != 0 ? 1 : 0;
    s.lateWarp    = s_params.lateWarp ? 1 : 0;
}

// Emit an EVENT row for each discrete config change since the last check, so the analyzer can pin a
// latency shift to "you toggled X". Cheap field compares each loop; floats use a tolerance so a slider
// drag logs a few rows, not hundreds. Continuously-driven values (auto trimMs/leadMs) are NOT events —
// they live in the STAT stream.
void CheckConfigEvents() {
    Telemetry::Sample c; FillConfig(c);
    if (!s_cfgInit) { s_lastCfg = c; s_cfgInit = true; Telemetry::Event(c, "session.begin"); return; }
    #define EV_I(field, label) if (c.field != s_lastCfg.field) Telemetry::Event(c, label "=%d", c.field)
    #define EV_F(field, label, tol) if (fabsf(c.field - s_lastCfg.field) > (tol)) Telemetry::Event(c, label "=%.3f", c.field)
    EV_I(enabled,     "warp.enable");
    EV_I(mode,        "warp.mode");
    EV_I(angular,     "warp.angular");
    EV_F(sens,        "warp.sens",       0.05f);
    EV_F(fov,         "warp.fov",        0.5f);
    EV_F(gain,        "warp.gain",       0.001f);
    if (c.sign != s_lastCfg.sign) Telemetry::Event(c, "warp.sign=%+.0f", c.sign);
    EV_I(autoTrim,    "warp.autotrim");
    EV_I(autoLead,    "pace.autolead");
    EV_F(leadFloorMs, "pace.leadfloor",  0.02f);
    EV_I(maxFif,      "pace.maxfif");
    EV_I(vsync,       "pace.vsync");
    EV_I(lateWarp,    "pace.latewarp");
    #undef EV_I
    #undef EV_F
    s_lastCfg = c;
}

void PresenterThread() {
    LOG_INFO("Presenter thread started (syncInterval=%d)", s_params.syncInterval);
    timeBeginPeriod(1);                 // 1 ms Sleep granularity for the pacing sleep
    s_lastVblankQpc = 0;
    s_refreshQpc    = QueryRefreshQpc();
    s_params.refreshHz = (float)((double)MouseTracker::MsToQpc(1000.0) / (double)s_refreshQpc);
    uint64_t lastStatQpc = MouseTracker::NowQpc();
    int      presentsInWindow = 0;
    uint64_t gameFramesAtWindow = s_params.gameFrames;
    double   sumInputAge = 0.0, sumGameAge = 0.0;   // QPC ticks, summed over the window
    int64_t  worstJitter = 0;                       // |interval - refresh|, max over the window
    uint64_t lastTuneQpc = lastStatQpc;             // adaptive-lead controller cadence
    uint64_t presentedAtLastTune = s_params.presented;  // for accurate per-tune drop count
    // Last GPU-complete game frame, kept resident so we can re-warp it without ever stalling on the
    // game queue (see the non-blocking frame selection below). Raw pointer — same lifetime contract as
    // s_latestFrame (the replacement-buffer pool outlives the brief window we hold it).
    ID3D12Resource* cachedGoodFrame = nullptr;
    uint64_t        cachedSubmitQpc = 0;

    while (s_running.load(std::memory_order_acquire)) {
        CheckConfigEvents();   // log any tunable the user just changed (cheap; writes only on change)

        // Snapshot the recent submission history (the whole ring; we index it newest-first below).
        FrameRec hist[kFrameHistory];
        uint64_t histCount;
        {
            std::lock_guard<std::mutex> lk(s_mtx);
            histCount = s_frameHistCount;
            for (int i = 0; i < kFrameHistory; ++i) hist[i] = s_frameHist[i];
        }

        // Non-blocking frame selection — flat pacing AND fresh frames below the refresh rate. When the
        // game is GPU-bound it runs 2-3 frames ahead of GPU completion, so its newest submission is
        // usually still rendering; checking only that one traps us on a stale buffer. Instead poll the
        // game fence once (CPU side, no GPU stall) and walk the history newest-first, taking the
        // FRESHEST frame the GPU has actually finished. Bound the walk to (bufferCount - 1): older
        // entries alias VRAM the game is already overwriting, so they are unsafe to read.
        UINT64 completed = s_gameFence->GetCompletedValue();
        int safeDepth = (int)s_bufferCount - 1;
        if (safeDepth < 1) safeDepth = 1;
        if (safeDepth > kFrameHistory) safeDepth = kFrameHistory;
        if (histCount > 0) {
            int avail = (int)(histCount < (uint64_t)kFrameHistory ? histCount : (uint64_t)kFrameHistory);
            int walk  = safeDepth < avail ? safeDepth : avail;
            for (int n = 0; n < walk; ++n) {
                const FrameRec& r = hist[(int)((histCount - 1 - (uint64_t)n) % kFrameHistory)];
                if (r.frame && completed >= r.fenceVal) {
                    cachedGoodFrame = r.frame;
                    cachedSubmitQpc = r.submitQpc;
                    break;   // freshest complete-and-safe frame
                }
            }
        }
        ID3D12Resource* frame = cachedGoodFrame;
        uint64_t submitQpc = cachedSubmitQpc;
        if (!frame) { ::Sleep(1); continue; }   // nothing fully rendered yet

        // Reflex-2-style late-warp pacing: sleep until just before the predicted next vblank, so the
        // mouse latch + warp below happen at the last possible moment (minimum input->photon latency)
        // and we present exactly once per refresh instead of spinning at ~1000 fps and starving the
        // game. The mouse is latched inside WarpInto/ReprojectInto via NowQpc() *after* this sleep.
        if (s_params.lateWarp && s_refreshQpc > 0 && s_lastVblankQpc) {
            int64_t  lead   = MouseTracker::MsToQpc(s_params.leadMs);
            // The warp budget equals `lead` (we wake this long before the vblank). It MUST stay
            // below a full refresh: if lead >= refresh, the `while` below skips an extra vblank and
            // `target - lead` lands microseconds before the *first* vblank, collapsing the budget to
            // ~0 and missing every frame. Clamp the effective lead so even a manual slider can't fall
            // off that cliff (the auto-controller is separately capped at 50% of the refresh).
            int64_t  maxLead = s_refreshQpc - s_refreshQpc / 10;   // 90% of a refresh, hard ceiling
            if (lead > maxLead) lead = maxLead;
            uint64_t now    = MouseTracker::NowQpc();
            uint64_t target = s_lastVblankQpc + (uint64_t)s_refreshQpc;
            while (target <= now + (uint64_t)lead) target += (uint64_t)s_refreshQpc;  // fell behind: skip ahead
            SleepUntilQpc(target - (uint64_t)lead);
        }

        // No GPU-timeline wait here: `frame` was confirmed GPU-complete by the CPU fence poll above
        // (a completed fence means the game's writes finished before we submit this warp), so the
        // present queue never stalls on the game queue.

        UINT ri = s_real->GetCurrentBackBufferIndex();
        ID3D12Resource* bb = nullptr;
        if (FAILED(s_real->GetBuffer(ri, IID_PPV_ARGS(&bb))) || !bb) { ::Sleep(1); continue; }

        // The warp latches the mouse via NowQpc() at the very start of WarpInto/ReprojectInto, so this
        // timestamp is the input-latch instant for the frame we are about to present.
        uint64_t warpStartQpc = MouseTracker::NowQpc();

        // Re-warp the freshest GPU-complete game frame (the replacement buffer the game rendered into)
        // with freshly late-latched mouse input, DIRECTLY into the real backbuffer — no capture copies,
        // no export ring. Mode 4 (perspective rotational) needs no depth/MV; FOV is the manual value.
        // Menu detection: suppress the warp (passthrough) in menus/pause (OS cursor visible) — there's
        // no camera motion to hide and warping would just swim the UI.
        WarpParams& wpRt = WarpRenderer::Params();
        wpRt.runtimeSuppress = wpRt.menuDetect && Overlay::InGameMenu();

        // Captured vertical FOV (radians) from the FSR dispatch. Drives auto-FOV (so the warp + angular
        // gain track the game's real/zoomed FOV) and FOV-based ADS detection. Sanity-guarded so junk
        // capture falls back to the manual FOV and never breaks the warp.
        DepthCapture::Cam cam; bool haveCam = DepthCapture::GetLatest(nullptr, nullptr, &cam);
        float capFov = haveCam ? cam.fovV : 0.0f;
        bool  fovSane = (capFov > 0.30f && capFov < 2.60f);   // ~17deg .. ~149deg
        if (fovSane) s_baseFov = (capFov > s_baseFov) ? capFov : (s_baseFov * 0.999f);  // hip-fire FOV = slow max
        wpRt.capturedFovDeg = fovSane ? capFov * 57.29578f : 0.0f;

        // ADS = the game zoomed in (FOV narrowed well below the hip-fire baseline).
        wpRt.adsActive = wpRt.adsForce ||
            (wpRt.adsDetect && fovSane && s_baseFov > 0.01f && capFov < wpRt.adsFovRatio * s_baseFov);

        float fovV = (wpRt.autoFov && fovSane) ? capFov : (wpRt.fovDeg * 3.14159265f / 180.0f);
        // Phase 1: feed the captured depth so mode-4 weapon/hand lock can keep the near-field
        // (gun + optics) screen-locked while the world reprojects. Null until the first FSR dispatch,
        // in which case the warp falls back to pure rotation (weapon-lock auto-off).
        ID3D12Resource* depthTex = nullptr; DXGI_FORMAT depthFmt = DXGI_FORMAT_UNKNOWN;
        DepthCapture::GetDepthSRV(&depthTex, &depthFmt);
        WarpRenderer::Instance().ReprojectInto(s_presentQueue,
                                               frame, D3D12_RESOURCE_STATE_PRESENT,
                                               bb,    D3D12_RESOURCE_STATE_PRESENT,
                                               depthTex, depthFmt,
                                               nullptr, DXGI_FORMAT_UNKNOWN,
                                               0.0f, fovV,
                                               nullptr, D3D12_RESOURCE_STATE_PRESENT,
                                               submitQpc);
        bb->Release();

        // Device-removal diagnostic: if the warp faulted the GPU, capture the reason once instead of
        // silently dying in the next D3D call.
        if (s_device) {
            HRESULT rr = s_device->GetDeviceRemovedReason();
            if (rr != S_OK) {
                static bool loggedRemoved = false;
                if (!loggedRemoved) {
                    loggedRemoved = true;
                    LOG_ERROR("Presenter: DEVICE REMOVED after warp reason=0x%08X", (unsigned)rr);
                }
            }
        }

        // Crisp (un-warped) menu on top of the warped frame.
        Overlay::RenderOverlay(s_real);

        HookManager::CallRealPresent(s_real, s_params.syncInterval, 0);
        s_params.presented++;
        presentsInWindow++;

        // Anchor the pacing phase to this present. With vsync on, the present blocked until the real
        // vblank, so this QPC ~= vblank and the inter-present interval ~= the refresh period — refine
        // the estimate via a slow EMA (and flag skipped refreshes for tuning). With vsync off we keep
        // the DWM-queried period (our immediate presents can't reveal it) and only use it as a rate cap.
        uint64_t vbl = MouseTracker::NowQpc();
        if (s_lastVblankQpc && s_refreshQpc > 0) {
            int64_t dt = (int64_t)(vbl - s_lastVblankQpc);
            // Real dropped frames are counted accurately per stat window below (expected minus actual
            // presents). This CPU-timestamped interval is NOT used for that anymore — it spikes whenever
            // the thread is briefly descheduled (esp. in menus/at startup) and massively over-reports.
            // Keep it only as an informational worst-interval ("jitter") indicator.
            int64_t jit = dt - s_refreshQpc; if (jit < 0) jit = -jit;
            if (jit > worstJitter) worstJitter = jit;
        }
        s_lastVblankQpc = vbl;

        // Latency telemetry. inputAge = latch -> scanout (with vsync, vbl ~= the real vblank/scanout),
        // which is exactly what late-warp drives down. gameAge = how stale the re-presented game frame
        // is at latch time (grows when present fps >> game fps — the inserted-frame extrapolation gap).
        sumInputAge += (double)(int64_t)(vbl - warpStartQpc);
        if (submitQpc && warpStartQpc > submitQpc) sumGameAge += (double)(int64_t)(warpStartQpc - submitQpc);

        // ~1 Hz stat window.
        uint64_t now = MouseTracker::NowQpc();
        double dt = (double)(now - lastStatQpc) / (double)MouseTracker::MsToQpc(1000.0);
        if (dt >= 1.0) {
            s_params.presentFps = (float)(presentsInWindow / dt);
            s_params.gameFps    = (float)((s_params.gameFrames - gameFramesAtWindow) / dt);
            // Re-query the authoritative display period from DWM (stable; no EMA drift) — handles a
            // refresh-rate change mid-session and keeps the HUD honest (no phantom 155 Hz).
            s_refreshQpc = QueryRefreshQpc();
            if (s_refreshQpc > 0)
                s_params.refreshHz = (float)((double)MouseTracker::MsToQpc(1000.0) / (double)s_refreshQpc);
            // Accurate dropped-frame count: expected presents (refresh x window) minus actual presents.
            // present_fps == refresh therefore reads ~0 dropped, which is the truth — unlike the old
            // per-interval CPU-timestamp test that over-reported tens/sec while present was locked.
            if (s_params.refreshHz > 0.0f) {
                long long dropped = (long long)((double)s_params.refreshHz * dt - (double)presentsInWindow + 0.5);
                if (dropped > 0) s_params.missedVblanks += (uint64_t)dropped;
            }
            double perMs = (double)MouseTracker::MsToQpc(1.0);
            if (presentsInWindow > 0) {
                s_params.inputAgeMs = (float)(sumInputAge / presentsInWindow / perMs);
                s_params.gameAgeMs  = (float)(sumGameAge  / presentsInWindow / perMs);
            }
            s_params.jitterMs = (float)(worstJitter / perMs);

            // Auto-trim: drive the warp's late-latch window to a fixed fraction of one game frame. The
            // warp extends its mouse-motion window back from the last present by trimMs, so the effective
            // window is (present interval - trimMs). Half a game frame is the average un-displayed camera
            // motion regardless of game/refresh rate — which is what -11 ms happened to be at 30/144. The
            // trimScale knob dials overall warp strength; this self-adjusts as the game's fps drifts.
            int64_t gi = s_gameIntervalQpc.load(std::memory_order_relaxed);
            if (WarpRenderer::Params().autoTrim && gi > 0 && s_refreshQpc > 0) {
                double gameMs       = (double)gi / perMs;
                double presentMs    = (double)s_refreshQpc / perMs;
                double targetWindow = (double)WarpRenderer::Params().trimScale * gameMs;
                float  trim = (float)(presentMs - targetWindow);
                if (trim < -40.0f) trim = -40.0f;   // stay within the manual slider's envelope
                if (trim >  40.0f) trim =  40.0f;
                WarpRenderer::Params().trimMs = trim;
            }

            // Runtime operating-point telemetry (1 Hz). Diagnoses warp feel: if gameAge >> the present
            // interval (1000/presentFps), the camera-warp term — referenced to the previous present —
            // under-covers the displayed frame's true staleness, which reads as decoupling/rubberband.
            {
                WarpParams& wp = WarpRenderer::Params();
                double presentIntervalMs = s_params.presentFps > 1.0f ? 1000.0 / s_params.presentFps : 0.0;
                LOG_INFO("RT: mode=%d en=%d ang=%d sens=%.2f sign=%+.0f trim=%.1f fov=%.0f | present=%.0f game=%.0f refresh=%.0f (interval=%.1fms) | inputAge=%.1f gameAge=%.1f jitter=%.1f | lead=%.2f(floor=%.2f) gpuDepth=%.1f fif=%d | warp=(%.4f,%.4f)",
                         wp.mode, wp.enable ? 1 : 0, wp.angularGain ? 1 : 0, wp.sensDegPer1000, wp.sign, wp.trimMs, wp.fovDeg,
                         s_params.presentFps, s_params.gameFps, s_params.refreshHz, presentIntervalMs,
                         s_params.inputAgeMs, s_params.gameAgeMs, s_params.jitterMs,
                         s_params.leadMs, s_params.leadFloorMs, s_params.gpuDepth, s_params.maxFramesInFlight,
                         wp.lastU, wp.lastV);
            }

            // Structured STAT row (config + this window's metrics) for offline analysis.
            {
                Telemetry::Sample ts; FillConfig(ts);
                ts.presentFps    = s_params.presentFps;
                ts.gameFps       = s_params.gameFps;
                ts.refreshHz     = s_params.refreshHz;
                ts.inputAgeMs    = s_params.inputAgeMs;
                ts.gameAgeMs     = s_params.gameAgeMs;
                ts.jitterMs      = s_params.jitterMs;
                ts.missedVblanks = s_params.missedVblanks;
                ts.gpuDepth      = s_params.gpuDepth;
                Telemetry::Stat(ts);
            }

            sumInputAge = sumGameAge = 0.0; worstJitter = 0;
            presentsInWindow = 0;
            gameFramesAtWindow = s_params.gameFrames;
            lastStatQpc = now;
        }

        // Adaptive vblank-lead: hunt the knee — the smallest lead that still covers the warp's GPU
        // cost. Creep down ~0.05 ms per quiet 250 ms window; back off proportionally the instant a
        // vblank slips. Settles into a tiny limit cycle right at the minimum-latency edge. (Only with
        // late-warp + vsync, where the lead governs how late we latch input.)
        if (s_params.autoLead && s_params.lateWarp) {
            if (now - lastTuneQpc >= (uint64_t)MouseTracker::MsToQpc(250.0)) {
                // Real drops in this tune window = expected minus actual presents (accurate, no
                // CPU-timestamp noise) so the controller stops backing off on phantom misses and can
                // creep the lead down to the true knee.
                double tuneSec = (double)(now - lastTuneQpc) / (double)MouseTracker::MsToQpc(1000.0);
                long long miss = (s_params.refreshHz > 0.0f)
                    ? (long long)((double)s_params.refreshHz * tuneSec - (double)(s_params.presented - presentedAtLastTune) + 0.5)
                    : 0;
                uint64_t windowMisses = miss > 0 ? (uint64_t)miss : 0;
                // Tolerate a couple of real slips per window before backing off; otherwise creep tighter.
                const uint64_t kTolerable = 2;
                if (windowMisses > kTolerable) s_params.leadMs += 0.20f * (float)(windowMisses - kTolerable);
                else                           s_params.leadMs -= 0.08f;  // creep tighter
                // Hard ceiling at HALF the measured refresh. The warp budget == leadMs, and crossing a
                // full refresh collapses it to ~0 (skips a vblank), which is the windup trap that pinned
                // lead at 8 ms with misses every frame. Half a refresh (e.g. 3.47 ms @144 Hz) is ample
                // headroom for a fullscreen warp and stays well clear of the cliff.
                float perMs     = (float)MouseTracker::MsToQpc(1.0);
                float refreshMs = (s_refreshQpc > 0) ? (float)((double)s_refreshQpc / perMs) : 6.94f;
                float maxLead   = refreshMs * 0.5f;
                float floorMs   = s_params.leadFloorMs > 0.05f ? s_params.leadFloorMs : 0.05f;
                if (floorMs > maxLead)         floorMs = maxLead;   // floor can't exceed the ceiling
                if (s_params.leadMs < floorMs) s_params.leadMs = floorMs;
                if (s_params.leadMs > maxLead) s_params.leadMs = maxLead;
                presentedAtLastTune = s_params.presented;
                lastTuneQpc = now;
            }
        }
    }
    timeEndPeriod(1);
    LOG_INFO("Presenter thread exiting");
}

} // namespace

bool AsyncEnabled() {
    // Async is the product now — default ON, no env flag required. Opt OUT with ASYNCREPROJ_ASYNC=0
    // (escape hatch back to the synchronous present-time warp if anything ever misbehaves).
    static int cached = -1;
    if (cached < 0) {
        wchar_t buf[8] = {};
        DWORD n = GetEnvironmentVariableW(L"ASYNCREPROJ_ASYNC", buf, 8);
        cached = (n > 0 && buf[0] == L'0') ? 0 : 1;   // present unless explicitly "0"
    }
    return cached == 1;
}

PresenterParams& Params() { return s_params; }

ID3D12CommandQueue* EnsureQueue(ID3D12Device* device) {
    if (s_presentQueue) return s_presentQueue;
    if (!device) return nullptr;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&s_presentQueue)))) {
        LOG_ERROR("Presenter: failed to create present queue");
        s_presentQueue = nullptr;
        return nullptr;
    }
    LOG_INFO("Presenter: created high-priority present queue 0x%p", s_presentQueue);
    return s_presentQueue;
}

void Start(IDXGISwapChain4* real, ID3D12CommandQueue* gameQueue, ID3D12Device* device) {
    if (s_running.load()) return;
    s_real = real;            s_real->AddRef();
    s_gameQueue = gameQueue;  s_gameQueue->AddRef();
    s_device = device;        s_device->AddRef();

    { DXGI_SWAP_CHAIN_DESC1 d = {};
      if (SUCCEEDED(real->GetDesc1(&d))) {
          s_dispW = (float)d.Width; s_dispH = (float)d.Height;
          if (d.BufferCount >= 2) s_bufferCount = d.BufferCount;   // == the replacement-pool size
      } }

    if (!s_gameFence) {
        s_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_gameFence));
        s_gameFenceVal = 0;
    }
    if (!s_drainFence) {
        s_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_drainFence));
        s_drainEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    if (!s_limiterEvent) s_limiterEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // The overlay records on the queue we present from, so it lands on the real backbuffer in order.
    Overlay::SetPresentQueue(s_presentQueue);

    { std::lock_guard<std::mutex> lk(s_mtx); s_haveFrame = false; s_latestFrame = nullptr;
      s_frameHistCount = 0; for (auto& r : s_frameHist) r = {}; }
    dc::Reset();
    s_running.store(true, std::memory_order_release);
    s_thread = std::thread(PresenterThread);
    LOG_INFO("Presenter: started on swapchain 0x%p (game queue 0x%p)", real, gameQueue);
    { Telemetry::Sample s; FillConfig(s); Telemetry::Event(s, "presenter.start"); }
}

void SubmitGameFrame(ID3D12Resource* frame) {
    if (!s_running.load(std::memory_order_acquire) || !frame) return;
    // Order a fence signal after the game's frame submissions on the game queue.
    UINT64 val = ++s_gameFenceVal;
    s_gameQueue->Signal(s_gameFence, val);

    // Frame-in-flight limiter: keep the game's CPU from racing ahead of GPU completion. Our Present
    // returns instantly (no swapchain backpressure), so a GPU-bound game would otherwise queue several
    // frames deep — the freshest GPU-complete frame ends up stale and the presenter re-warps an old
    // buffer (the rubberbanding). Block here until at most `maxFramesInFlight` frames are outstanding,
    // which both caps game-age and keeps a just-finished frame inside the presenter's safe search depth.
    // Timeout guards against a stalled GPU (we'd rather run loose than deadlock the game thread).
    int cap = s_params.maxFramesInFlight;
    if (cap > 0 && s_limiterEvent && val > (UINT64)cap) {
        UINT64 floor = val - (UINT64)cap;
        if (s_gameFence->GetCompletedValue() < floor) {
            s_gameFence->SetEventOnCompletion(floor, s_limiterEvent);
            WaitForSingleObject(s_limiterEvent, 100);   // ~6 refreshes @180 Hz; never hang the game
        }
    }
    s_params.gpuDepth = (float)(val - s_gameFence->GetCompletedValue());

    // Track the game-frame interval (smoothed) so the MV factor knows one game frame's worth of time.
    uint64_t nowq = MouseTracker::NowQpc();
    uint64_t prev = s_lastGameQpc.exchange(nowq, std::memory_order_relaxed);
    if (prev && nowq > prev) {
        int64_t dt = (int64_t)(nowq - prev);
        int64_t old = s_gameIntervalQpc.load(std::memory_order_relaxed);
        s_gameIntervalQpc.store(old ? (old * 7 + dt) / 8 : dt, std::memory_order_relaxed);  // EMA
    }
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_latestFrame = frame;
        s_latestFenceVal = val;
        s_latestSubmitQpc = nowq;
        s_haveFrame = true;
        s_frameHist[s_frameHistCount % kFrameHistory] = { frame, val, nowq };
        s_frameHistCount++;
    }
    s_params.gameFrames++;

    // Adaptive low-latency delay: hold the game thread here (its frame boundary) a tuned amount so the
    // next frame samples input later -> fresher content. Off = current fixed-cap behaviour (for A/B).
    if (s_params.adaptiveDelay) {
        dc::Delay();
        s_params.drainMs     = (float)((double)dc::drain / (double)MouseTracker::MsToQpc(1.0));
        s_params.simGradient = (float)dc::gradientEwma;
    } else {
        dc::Reset();
        s_params.drainMs = 0.0f;
    }
}

void Stop() {
    if (!s_running.exchange(false)) {
        // Not running; still release any held swapchain refs.
    } else if (s_thread.joinable()) {
        s_thread.join();
        { Telemetry::Sample s; FillConfig(s); Telemetry::Event(s, "presenter.stop"); }
    }
    // Drain the present queue so no in-flight warp still references the replacement buffers.
    if (s_presentQueue && s_drainFence && s_drainEvent) {
        s_presentQueue->Signal(s_drainFence, ++s_drainVal);
        if (s_drainFence->GetCompletedValue() < s_drainVal) {
            s_drainFence->SetEventOnCompletion(s_drainVal, s_drainEvent);
            WaitForSingleObject(s_drainEvent, INFINITE);
        }
    }
    { std::lock_guard<std::mutex> lk(s_mtx); s_haveFrame = false; s_latestFrame = nullptr;
      s_frameHistCount = 0; for (auto& r : s_frameHist) r = {}; }
    if (s_real)      { s_real->Release();      s_real = nullptr; }
    if (s_gameQueue) { s_gameQueue->Release(); s_gameQueue = nullptr; }
    if (s_device)    { s_device->Release();    s_device = nullptr; }
}

void Shutdown() {
    Stop();
    if (s_gameFence)  { s_gameFence->Release();  s_gameFence = nullptr; }
    if (s_drainFence) { s_drainFence->Release(); s_drainFence = nullptr; }
    if (s_drainEvent) { CloseHandle(s_drainEvent); s_drainEvent = nullptr; }
    if (s_limiterEvent) { CloseHandle(s_limiterEvent); s_limiterEvent = nullptr; }
    if (s_waitTimer)    { CloseHandle(s_waitTimer);    s_waitTimer = nullptr; }
    if (s_presentQueue) { s_presentQueue->Release(); s_presentQueue = nullptr; }
}

} // namespace Presenter
