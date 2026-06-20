#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <cstdint>

// In-process reprojection warp (P3, step 1: present-time "frame warp").
//
// Renders a reprojected copy of the just-rendered backbuffer back into the backbuffer, shifted by
// the mouse motion that occurred since the game sampled input for this frame (late-latched from
// MouseTracker). This is the latency-reducing rotational warp; later it gains MV/depth terms and
// moves onto the async presenter thread.
//
// Tunables are exposed so the overlay menu can drive them live.
struct WarpParams {
    bool  enable   = false;
    int   mode     = 0;        // 0 = rotational shift; 2 = hybrid (mouse+MV objects); 3 = per-pixel MV; 4 = perspective rotational
    float gain     = 0.0673f;  // mouse counts -> fraction of screen, per the v1 calibration
    float sign     = 1.0f;     // warp direction (flipped from -1: -1 sent the world the wrong way)
    float trimMs   = -11.0f;   // lead(+)/lag(-) applied to the base timestamp

    // ---- trim auto-calibration (presenter drives trimMs from the measured game-frame interval) ----
    // The warp's effective late-latch window is (present interval - trimMs). Targeting a fixed fraction
    // of one game frame keeps the warp showing the average un-displayed camera motion at any game/refresh
    // rate (-11 ms happened to be ~half a frame at 30 fps / 144 Hz). trimScale dials overall strength.
    bool  autoTrim  = true;    // presenter sets trimMs each stat window; manual slider hidden when on
    float trimScale = 0.5f;    // fraction of a game frame to lead by (0.5 = half-frame; lower = gentler)

    // ---- gain auto-calibration (regress far-corner MV vs mouse delta; see FeedCalibration) ----
    bool  autoCalibrate = true;   // continuously drive `gain` to the measured MV<->mouse slope
    float calScale      = 0.8f;   // user trim on the applied auto-gain (auto-cal reads a touch strong)
    // Readouts (written by FeedCalibration, read by the overlay). detectedSign is informational
    // only — we never auto-flip `sign` (a wrong sign sends the warp backwards = nausea).
    float calGain       = 0.0f;   // current converged |slope| (what `gain` is set to when applied)
    float detectedSign  = 0.0f;   // sign of the raw slope (+1/-1), for the user to match `sign` to
    float calConfidence = 0.0f;   // 0..1 ramp with sample count (overlay shows convergence)
    int   calSamples    = 0;      // accumulated significant-motion samples

    // ---- mode 2 (depth + MV reprojection) tunables ----
    float mvScale       = 1.0f;    // MV -> UV gain (the probe showed MV ~ UV space, so ~1)
    float mvThreshold   = 0.001f;  // residual-MV magnitude above which a pixel counts as a moving object
    float depthEdgeThresh = 0.01f; // reversed-Z raw depth gap that marks a disocclusion (skip reproject)
    bool  depthEdge     = true;    // depth-aware disocclusion guard on/off
    float nearDepthCut  = 0.85f;   // reversed-Z depth above this = near field (weapon): no MV reproject
    float camRejectK    = 1.5f;    // camera-translation false-positive rejection strength (higher = stricter)
    bool  weaponLock    = true;    // mode 4: keep near-field (weapon + optics) screen-locked, warp only the world
    float weaponDilate  = 0.05f;   // mode 4: UV radius to fill weapon-mask holes (scope lens at world depth)

    // ---- HUD lock (region-based; flat UI has no usable depth) — superseded by the hud-less pivot,
    // kept as a fallback. Default off (the visible circle/rectangle looks worse than the swim). ----
    bool  hudMask    = false;      // keep screen-anchored UI from warping (a fixed reference)
    float hudCenterR = 0.05f;      // crosshair lock radius (UV, circular about center)
    float hudEdge    = 0.0f;       // edge-HUD lock inset (UV from each edge; minimap/ammo/health)

    // ---- HUD-less compositor (Stage 1): the warp source is the FINAL present buffer with UI holes
    // filled from the hud-less buffer; the real UI is re-applied unwarped as a screen-space layer.
    // uiThreshold = |present - hudless| (max channel) above which a pixel is treated as UI; the mask
    // ramps from uiThreshold to 2x it. Raise it if film grain leaks UI; lower it if faint UI is lost.
    float uiThreshold = 0.04f;
    int   uiErode     = 1;       // UI-mask erosion radius (px); 0 = single-tap. Rejects grain speckle
                                 // that otherwise ghosts the unwarped present into the warped scene.
    bool  debugMask   = false;   // visualize the UI mask as grayscale (tuning)
    bool  hudCompose  = true;    // false = skip hud-less separation, warp the final frame as one layer
                                 // (HUD swims, but no mask/ghost) — clean low-latency baseline.

    // Filled by the renderer for the overlay HUD.
    float lastU = 0.0f, lastV = 0.0f;   // last rotational offset (UV)
    float lastMvFactor = 0.0f;          // last MV extrapolation factor (game-frames into the future)
};

class WarpRenderer {
public:
    static WarpRenderer& Instance();
    static WarpParams& Params();

    // Runs the warp pass into `backbuffer` on `queue`, before the overlay draws and before present.
    // No-op (returns immediately) when the warp is disabled. Lazily builds all D3D12 objects.
    // Used by the synchronous present-time path (proxy Present), where source == backbuffer so a
    // copy-to-scratch is needed first.
    void Render(ID3D12CommandQueue* queue, ID3D12Resource* backbuffer);

    // Async-presenter path: warp `source` (the game's replacement backbuffer, left in `srcState`)
    // into `dest` (the real swapchain backbuffer, left in `destState`) on `queue`, no scratch copy.
    // When the warp is disabled this still runs with a zero offset, so the presenter always produces
    // a presentable frame (effectively a resample/passthrough). All barriers return the resources to
    // their incoming states. Caller is responsible for any cross-queue fence wait on `source`.
    // frameSubmitQpc: QPC when the game submitted the frame being warped. When non-zero the late-latch
    // window is measured from THIS timestamp (so the warp accumulates with the frame's true age across
    // re-presents) instead of from the previous present. 0 = legacy present-relative (sync path).
    void WarpInto(ID3D12CommandQueue* queue,
                  ID3D12Resource* source, D3D12_RESOURCE_STATES srcState,
                  ID3D12Resource* dest,   D3D12_RESOURCE_STATES destState,
                  uint64_t frameSubmitQpc = 0);

    // P4 hybrid reprojection (mode 2): warp `color` (replacement backbuffer, in `srcState`) into
    // `dest` (real backbuffer, in `destState`), using `depth` + `mv` (from the FSR capture, already
    // in COMMON/simultaneous-access — bound as SRVs with no barrier) to reproject moving objects on
    // top of the late-latched mouse camera warp. `mvFactor` is the async extrapolation factor
    // (game-frames elapsed since the captured frame). Falls back to the rotational path if depth/mv
    // are null. Caller fences `color` (cross-queue) before calling.
    void ReprojectInto(ID3D12CommandQueue* queue,
                       ID3D12Resource* color, D3D12_RESOURCE_STATES srcState,
                       ID3D12Resource* dest,  D3D12_RESOURCE_STATES destState,
                       ID3D12Resource* depth, DXGI_FORMAT depthSrvFmt,
                       ID3D12Resource* mv,    DXGI_FORMAT mvSrvFmt,
                       float mvFactor, float fovV,
                       ID3D12Resource* hud = nullptr, D3D12_RESOURCE_STATES hudState = D3D12_RESOURCE_STATE_PRESENT,
                       uint64_t frameSubmitQpc = 0);

    // Gain auto-calibration. Called once per game frame with the corner motion-vector (UV, far-field
    // = pure camera screen motion) and the mouse delta over that same frame interval. Runs a
    // decayed least-squares regression of cornerMV against mouseDelta/screenW; when converged, sets
    // Params().gain = |slope|. The sign is reported but never applied automatically. No-op unless
    // Params().autoCalibrate is on. screenW/H are the display dimensions for normalizing the delta.
    void FeedCalibration(float mvU, float mvV, float mdx, float mdy, float screenW, float screenH);

    void Shutdown();
};
