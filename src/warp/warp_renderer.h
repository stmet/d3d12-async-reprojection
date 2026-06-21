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
    // Menu detection: when the game releases its cursor clip (inventory/pause/dialogue/main menu), the
    // camera isn't moving, so warping just makes the UI swim. menuDetect auto-suppresses the warp in
    // those states (the presenter sets runtimeSuppress from the game's cursor-clip state each frame).
    // Heuristic — toggle off if a game doesn't clip the cursor during gameplay.
    bool  menuDetect      = true;
    bool  runtimeSuppress = false;  // set by the presenter; warp behaves as disabled this frame when true
    int   mode     = 4;        // lean default: 4 = perspective rotational (the proven low-latency warp)
    float gain     = 0.0673f;  // mouse counts -> fraction of screen, per the v1 calibration
    float sign     = 1.0f;     // warp direction (flipped from -1: -1 sent the world the wrong way)
    float trimMs   = -11.0f;   // lead(+)/lag(-) applied to the base timestamp

    // ---- trim auto-calibration (presenter drives trimMs from the measured game-frame interval) ----
    // The warp's effective late-latch window is (present interval - trimMs). Targeting a fixed fraction
    // of one game frame keeps the warp showing the average un-displayed camera motion at any game/refresh
    // rate (-11 ms happened to be ~half a frame at 30 fps / 144 Hz). trimScale dials overall strength.
    bool  autoTrim  = true;    // presenter sets trimMs each stat window; manual slider hidden when on
    float trimScale = 0.5f;    // fraction of a game frame to lead by (0.5 = half-frame; lower = gentler)

    // ---- angular gain model (lean default, mode 4): the FOV-correct mapping from mouse counts to
    // camera rotation. yaw_radians = counts * (sensDegPer1000 deg / 1000 counts) * (pi/180). This
    // constant is purely the game's mouse sensitivity (the game's yaw-per-count); DPI is already baked
    // into the raw counts we read and is also baked into the counts the GAME reads, so it cancels — DPI
    // is NOT a separate input. FOV is handled AUTOMATICALLY by the mode-4 perspective projection: a
    // fixed deg/count produces a larger on-screen shift as the FOV narrows (ADS/zoom), which is exactly
    // correct, so gain never needs re-tuning per situation. This replaces the old measure-the-slope
    // auto-gain (which ran a noisy closed loop on a VISIBLE parameter -> the breathing/micro-rubberband).
    // Calibrate this ONE number once (by eye), then it stays frozen. ----
    bool  angularGain    = true;    // mode 4: use the deg/count model below instead of the flat UV `gain`
    float sensDegPer1000 = 3.0f;    // camera yaw degrees per 1000 mouse counts (~matches the v1 gain @59 deg)
    float pitchRatio     = 1.0f;    // vertical:horizontal sensitivity ratio (most FPS games = 1.0)

    // ---- gain auto-calibration: REMOVED in the lean build (no MV capture). Manual gain only. ----
    bool  autoCalibrate = false;  // kept for overlay compat; no effect (no calibration source)
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
    float nearDepthCut  = 0.95f;   // reversed-Z depth above this = near field (weapon): no MV reproject
    float camRejectK    = 1.5f;    // camera-translation false-positive rejection strength (higher = stricter)
    bool  weaponLock    = true;    // mode 4: keep near-field (weapon + optics) screen-locked, warp only the world
    float weaponDilate  = 0.095f;  // mode 4: UV radius to fill weapon-mask holes (scope lens at world depth)
    float maskDilate    = 0.006f;  // mode 4: grow the near-mask by this UV radius to cover the soft render-res
                                   // depth silhouette edge — kills the ghost-outline of the gun (0 = off)
    float fovDeg        = 59.0f;   // manual vertical FOV (deg) for the mode-4 perspective warp (lean build
                                   // has no FSR dispatch capture to read it from — tune to match the game)
    // ---- disocclusion / fast-flick edge handling (mode 4) ----
    // We have no render guard band (only the final frame), so rotating past the frame edge has no source
    // data. maxWarpDeg caps the per-present rotation so the disoccluded band can't get huge on fast
    // flicks (the eye is motion-blurred then anyway); edgeFade softens whatever band remains to black
    // instead of smearing the clamped border pixel across it.
    // Both default OFF: the plain clamp-smear (no black margin, no magnitude cap) is what felt best.
    // maxWarpDeg clamps yaw/pitch PER AXIS, which distorts diagonal/circular motion when it triggers —
    // leave at 0 unless hard-flick smear specifically bothers you.
    float maxWarpDeg    = 0.0f;    // 0 = uncapped; >0 clamps |yaw|,|pitch| per present (deg)
    float edgeFade      = 0.0f;    // 0 = off (clamp smear); >0 = fade out-of-frame samples to black over this UV width

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

    // Async-compute warp (the VR-compositor decoupling): warp `color` (replacement backbuffer) into
    // `dest` (the REAL backbuffer, which MUST be created with DXGI_USAGE_UNORDERED_ACCESS) on a
    // dedicated COMPUTE queue, writing the destination via a UAV instead of a graphics RTV draw. This
    // takes the warp off the game's direct-graphics submission path so the GPU scheduler can interleave
    // it instead of serializing it behind the game's whole frame (which is what tied present rate to
    // game rate). Records + executes on `computeQueue` and signals an internal fence; the caller must
    // make its present queue Wait() on (*outFence, *outFenceValue) before presenting. Mode 4 / mode 0
    // only (lean; no depth/mv/hud). Returns false if the compute pipeline can't init -> caller falls
    // back to ReprojectInto on the graphics queue.
    void Shutdown();
};
