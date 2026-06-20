#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <cstdint>

// Hud-less render-target tracker (Track A, step 1 — OptiScaler hudfix-style, slimmed for Cyberpunk).
//
// Goal: capture the scene as it exists BEFORE the flat 2D HUD is drawn, so the presenter can warp the
// world (and the world-anchored 3D markers/dialogs/optics baked into it) while the un-warped UI is
// composited on top later. We find that buffer by tracking the game's render targets:
//   - Hook ID3D12Device::CreateRenderTargetView to map each RTV descriptor handle -> its resource.
//   - Hook ID3D12GraphicsCommandList::OMSetRenderTargets to see which RT is bound; when a swapchain-
//     matching color RT is UNBOUND (the game finished rendering the scene into it and switched away),
//     copy its content into our hud-less texture on the game's own command list (correct GPU ordering,
//     known RENDER_TARGET state). The last such copy before Present wins.
//
// VALIDATION-FIRST: this only fills a texture the overlay debug view samples. Nothing is wired into
// the warp yet. Entirely gated behind ASYNCREPROJ_HUDLESS=1 (default off) so the shipping build is
// untouched; the vtable hooks are installed lazily only when enabled.
namespace RtTracker {

// env ASYNCREPROJ_HUDLESS=1, checked once. When false every entry point is a no-op.
bool Enabled();

// Swapchain backbuffer dimensions/format — the signature of a "full-frame" color RT candidate.
void SetSwapchainInfo(uint32_t width, uint32_t height, DXGI_FORMAT fmt);

// Register/forget targets to EXCLUDE from hud-less candidates: the proxy's replacement backbuffers
// (the game composites the final UI frame into these) and the real swapchain backbuffers (our own
// warp/overlay render into these). The hud-less candidate is a DIFFERENT, game-created texture.
void RegisterExcludedTarget(ID3D12Resource* buf);
void ClearExcludedTargets();

// Install the device + command-list vtable hooks (once). Called at the first FSR dispatch, where we
// hold both a device and one of the game's direct command lists (whose vtable is shared by all).
void EnsureHooks(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList);

// Called at each FSR upscale dispatch ("scene is done, post-FX/UI to follow"). Arms the
// first-after-upscale capture mode so we snapshot the post-FX scene before the UI is drawn.
void OnUpscaleDispatch();

// Capture point: 0 = first qualifying unbind AFTER the upscale (post-FX scene, pre-UI — default);
// 1 = last qualifying unbind before present (post-UI; what showed the HUD baked in). Live-tunable
// from the overlay so the user can compare in the debug view.
void SetCapturePoint(int point);
int  GetCapturePoint();

// The captured hud-less texture (COMMON / simultaneous-access — sample directly via SRV). Null until a
// candidate has been captured. Format/size match the swapchain.
ID3D12Resource* GetHudless();
DXGI_FORMAT     GetHudlessFormat();
void            GetHudlessSize(uint32_t& w, uint32_t& h);

// Overlay telemetry.
struct Stats {
    bool     hooksInstalled = false;
    uint32_t rtvMapSize     = 0;   // distinct RTV handles seen
    uint32_t candidatesLastFrame = 0;
    uint64_t captures       = 0;   // total hud-less copies issued
    uint32_t lastCandidateW = 0, lastCandidateH = 0;
    uint32_t lastCandidateFmt = 0;
};
Stats GetStats();

} // namespace RtTracker
