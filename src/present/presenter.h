#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdint>

// Async presenter (P2). The engine that makes the reprojection asynchronous from the game's
// render rate.
//
// In async mode the game no longer presents its own frames. It renders into off-screen
// "replacement" backbuffers handed to it by the proxy swapchain (FFX FrameInterpolationSwapChain
// model). On the game's Present, the proxy calls SubmitGameFrame() with the buffer it just
// finished, and returns immediately — the game keeps running at its own cap. A dedicated presenter
// thread owns the REAL swapchain's presents: every display refresh it takes the latest game frame,
// re-warps it with freshly late-latched mouse input, and presents. Between two game frames it emits
// several warped frames, each tracking newer input than the one before — that is the async win.
//
// The real swapchain is created on the presenter's own queue (not the game's) so DXGI orders each
// Present after the presenter's warp work, never against the game's rendering.
//
// Opt-in: only active when ASYNCREPROJ_ASYNC=1 at launch (read once by the proxy). Otherwise the
// proxy stays on the validated synchronous present-time warp and the presenter is never started.
namespace Presenter {

// True if ASYNCREPROJ_ASYNC=1 — checked once, cached. Decides at swapchain creation whether the
// proxy uses replacement buffers + the presenter, or the synchronous pass-through warp.
bool AsyncEnabled();

// Live tunables surfaced in the overlay menu (separate from WarpParams, which still drive the warp
// math). syncInterval 1 = vsync-locked presents (tear-free, paced by refresh); 0 = uncapped.
struct PresenterParams {
    int  syncInterval = 1;
    // Reflex-2-style late-warp pacing: sleep until just before the predicted vblank, THEN latch the
    // mouse + warp + present. Caps presents to the refresh (no 1000 fps starvation) and minimises the
    // input->photon latency by warping at the last possible moment.
    bool  lateWarp = true;
    bool  autoLead = true;      // closed-loop: drive leadMs to the knee (lowest latency, ~0 missed vblanks)
    float leadMs   = 2.5f;      // wake this long before the predicted vblank (must cover warp GPU time)
    float leadFloorMs = 0.3f;   // auto-lead never creeps below this (lower = chase latency harder, more slip risk)
    // Frame-in-flight limiter: cap how far the game's CPU may run ahead of GPU completion. With our
    // non-blocking Present the game would otherwise race several frames ahead (GPU-bound), so the
    // freshest GPU-complete frame is stale -> the warp re-uses an old buffer (rubberbanding). Capping
    // it both lowers game-age AND keeps a freshly-finished frame inside the presenter's safe window.
    int   maxFramesInFlight = 1; // game submit blocks until GPU is within this many frames (0 = off)
    // Adaptive game-thread delay (Anti-Lag/Reflex-style, ported from low_latency_layer). Injects a
    // small CPU delay at the game's Present each frame, tuned by a jitter+EWMA-gradient probe, to push
    // the (decoupled) simulation to its minimal-queue floor -> later input sampling -> fresher content.
    bool  adaptiveDelay = false; // POC toggle (off = current fixed-cap behaviour, for A/B)
    float drainMs       = 0.0f;  // current adaptive delay applied per game frame (telemetry)
    float simGradient   = 0.0f;  // EWMA gradient: ~1 = at the sim floor, ~0 = backlogged (telemetry)
    // HUD counters (written by the presenter thread, read by the menu).
    float presentFps  = 0.0f;   // real presents/sec (the displayed rate)
    float gameFps     = 0.0f;   // game frames handed off/sec (the render rate)
    float refreshHz   = 0.0f;   // measured/queried display refresh used for pacing
    // Latency telemetry (windowed averages, written by the presenter thread).
    float inputAgeMs  = 0.0f;   // mouse latch -> scanout: what late-warp minimises (vsync: true scanout)
    float gameAgeMs   = 0.0f;   // age of the game content being re-presented, at warp time
    float jitterMs    = 0.0f;   // worst present-interval deviation from the refresh in the window
    float gpuDepth    = 0.0f;   // game frames the CPU leads GPU completion by (pipeline depth)
    uint64_t presented = 0;     // total presenter presents
    uint64_t gameFrames = 0;    // total game frames submitted
    uint64_t missedVblanks = 0; // presents that slipped a refresh (lead too small / warp too slow)
};
PresenterParams& Params();

// Create the presenter's command queue (lazily, once) on `device`. Returned queue is owned by the
// presenter; the caller passes it to the real CreateSwapChain so the swapchain presents on it.
// Returns nullptr on failure (caller then falls back to the game queue / synchronous path).
ID3D12CommandQueue* EnsureQueue(ID3D12Device* device);

// Start the presenter thread against the real swapchain `real` (created on EnsureQueue's queue).
// `gameQueue` is the game's own queue, fenced so the presenter knows when each game frame is done.
// Safe to call once per swapchain; Stop() before the swapchain is resized/destroyed.
void Start(IDXGISwapChain4* real, ID3D12CommandQueue* gameQueue, ID3D12Device* device);

// Called from the proxy's Present on the game thread: `frame` is the replacement backbuffer the
// game just finished rendering. Records it as the latest frame for the presenter to warp.
void SubmitGameFrame(ID3D12Resource* frame);

// Stop the presenter thread and drain. The queue is kept (reused across resizes). Call before
// ResizeBuffers / swapchain teardown.
void Stop();

// Full teardown including the queue. Call at DLL detach.
void Shutdown();

} // namespace Presenter
