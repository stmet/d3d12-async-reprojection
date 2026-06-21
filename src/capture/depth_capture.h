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

} // namespace DepthCapture
