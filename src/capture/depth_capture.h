#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <cstdint>

// Lean depth/MV capture for the depth-aware reprojection roadmap (weapon-lock, parallax). Self-
// contained: detours LoadLibrary so it can late-hook the FSR3.1 ffx-api DLL (amd_fidelityfx_dx12.dll)
// the moment the game loads it, then detours ffxDispatch and reads the upscale dispatch's depth /
// motion-vector / camera inputs. Phase 0 only *intercepts and logs* — it proves the source is real
// before the presenter copies the depth and the warp consumes it.
namespace DepthCapture {

struct Cam {
    float    nearZ = 0.0f, farZ = 0.0f, fovV = 0.0f;   // camera near/far planes, vertical FOV (radians)
    uint32_t renderW = 0, renderH = 0;                  // FSR input (render) resolution
};

// Install/remove the LoadLibrary + ffxDispatch detours. Safe to call once from HookManager.
void Install();
void Uninstall();

// Called by the ffxDispatch hook for each upscale dispatch with the game's live depth/MV resources
// (in their FSR-input state) and camera params. Stores the latest; logs when they change.
void OnUpscaleDispatch(ID3D12Resource* depth, ID3D12Resource* mv, const Cam& cam,
                       uint32_t depthFfxState, uint32_t mvFfxState);

// Latest intercepted resources for consumers (the presenter, later). Returns false if nothing
// captured yet. Raw pointers — valid only as long as the game keeps the resources alive; the
// presenter must copy under the same frame it reads (a fence-gated copy, added in the next phase).
bool GetLatest(ID3D12Resource** depth, ID3D12Resource** mv, Cam* cam);

// Our own stable copy of the latest depth (ALLOW_SIMULTANEOUS_ACCESS, render resolution) for the
// presenter/warp to sample as an SRV from COMMON with no barrier. Returns false until the first copy.
// `srvFmt` is the SRV-compatible format (e.g. R32_FLOAT for a D32 depth).
bool GetDepthSRV(ID3D12Resource** tex, DXGI_FORMAT* srvFmt);

// Our stable copy of the latest motion-vector field (render res, SIMULTANEOUS_ACCESS) for the camera-
// translation fit. Returns false until the first copy. MV is a concrete color format (e.g. R16G16_FLOAT).
bool GetMvSRV(ID3D12Resource** tex, DXGI_FORMAT* srvFmt);

// ---- Phase 3 (MV-as-sensor): global camera-translation estimate ----
// The MV field encodes per-pixel screen motion = camera rotation + camera translation (parallax) +
// object motion. We DON'T warp it per-pixel (that's the "soup"); instead we use it once per frame as a
// low-dim sensor: subtract the known rotational flow, then solve a depth-weighted least-squares for the
// single 3D camera translation that best explains the residual parallax (near pixels move more than far
// — that depth dependence is what makes the 3 unknowns observable). Pass the game's per-present rotation
// delta (radians) to remove rotation; pass 0,0 to fit RAW MV (validate first with a no-look strafe/walk).
// Runs once per present on the present (direct) queue; result is read back a few frames latent (no stall).
// nearCut = reversed-Z near threshold (excludes the weapon). Camera near/far/FOV come from the capture.
void ComputeCameraTranslation(ID3D12CommandQueue* queue, float yawDelta, float pitchDelta, float nearCut);
// Latest fitted translation (view-space, units per game-frame: x=right, y=up, z=forward) and a 0..1
// confidence (ramps with the inlier sample count). xyz stay 0 until the first solve.
void GetCameraTranslation(float out3[3], float* confidence);

} // namespace DepthCapture
